/*
* sdrgg.h - Public API for libsdrgg
*
* A minimal, zero-copy SDR hardware access layer for Linux.
* Provides direct register access to RTL2832U + attached tuner.
* No abstraction barriers - full low-level control exposed.
*
* Architecture:
*   USB transport (usbdevfs async URB) -> RTL2832U demod chip -> tuner I2C
*   Zero-copy: page-aligned DMA buffers via USBDEVFS_SUBMITURB
*   Multi-device: single epoll event loop thread services all dongles
*   Timestamps: CLOCK_MONOTONIC for cross-device coherence
*
* License: MIT
* Author: Luigi Origa (2026)
*/

#ifndef SDRGG_H
#define SDRGG_H

/* ---- Library version ---- */
#define SDRGG_VERSION_MAJOR  1
#define SDRGG_VERSION_MINOR  3
#define SDRGG_VERSION_PATCH  0

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Error codes ---- */
#define SDRGG_OK             0
#define SDRGG_ERR_USB       -1
#define SDRGG_ERR_IO        -2
#define SDRGG_ERR_NODEV     -3
#define SDRGG_ERR_BUSY      -4
#define SDRGG_ERR_TIMEOUT   -5
#define SDRGG_ERR_PLL       -6
#define SDRGG_ERR_PARAM     -7
#define SDRGG_ERR_NOMEM     -8

/* ---- Tuner type identification ---- */
typedef enum {
  SDRGG_TUNER_UNKNOWN = 0,
  SDRGG_TUNER_R820T,
  SDRGG_TUNER_R820T2,
  SDRGG_TUNER_FC0012,
  SDRGG_TUNER_FC0013,
  SDRGG_TUNER_FC2580,
  SDRGG_TUNER_E4000,
} sdrgg_tuner_type_t;

/* ---- Device handle (opaque) ---- */
typedef struct sdrgg_dev sdrgg_dev_t;

/* ---- Multi-device context (opaque) ---- */
typedef struct sdrgg_ctx sdrgg_ctx_t;

/* ---- IQ buffer descriptor ---- */
typedef struct {
  uint8_t *data; /* pointer to IQ samples ( I,Q,I,Q,... ) unsigned 8-bit */
  uint32_t length; /* valid bytes in this buffer */
  uint64_t timestamp_us; /* monotonic timestamp at buffer completion ( microseconds ) */
  uint32_t sequence; /* buffer sequence number ( for drop detection ) */
} sdrgg_buffer_t;

/* ---- Streaming callback ---- */
typedef void ( *sdrgg_stream_cb_t )( sdrgg_dev_t *dev, const sdrgg_buffer_t *buf, void *user_ctx );

/* ---- Device info (filled by enumerate or open) ---- */
typedef struct {
  char path[64]; /* USB device path e.g. "/dev/bus/usb/001/004" */
  uint16_t vid; /* USB vendor ID */
  uint16_t pid; /* USB product ID */
  char serial[64]; /* device serial string ( enumerate only ) */
  uint8_t bus; /* USB bus number */
  uint8_t devaddr; /* USB device address on bus */
  sdrgg_tuner_type_t tuner;/* detected tuner type */
} sdrgg_devinfo_t;

/* ---- Streaming configuration ---- */
typedef struct {
  uint32_t buf_count; /* number of USB transfer buffers ( default: 32 ) */
  uint32_t buf_size; /* size of each buffer in bytes ( default: 16384 ) */
} sdrgg_stream_cfg_t;

/* ========== Device enumeration (C linkage) ========== */

/* Find all connected RTL2832U devices.
* Returns number of devices found. Info written to devs[] array.
* max_devs: size of devs[] array */
int32_t sdrgg_enumerate( sdrgg_ctx_t *ctx, sdrgg_devinfo_t *devs, int32_t max_devs );

#ifdef __cplusplus
} /* extern "C" */

/* ========== Demod register access namespace ========== */

namespace demod {

int32_t write( sdrgg_dev_t *dev, uint8_t block, uint16_t reg, uint8_t val );
int32_t read( sdrgg_dev_t *dev, uint8_t block, uint16_t reg, uint8_t *val );
int32_t write_bulk( sdrgg_dev_t *dev, uint8_t block, uint16_t reg, const uint8_t *data, uint16_t len );
int32_t read_bulk( sdrgg_dev_t *dev, uint8_t block, uint16_t reg, uint8_t *data, uint16_t len );

} /* namespace demod */

/* ========== Tuner I2C namespace ========== */

namespace tuner {

int32_t write( sdrgg_dev_t *dev, const uint8_t *data, uint8_t len );
int32_t read( sdrgg_dev_t *dev, uint8_t reg, uint8_t *data, uint8_t len );
int32_t write_reg( sdrgg_dev_t *dev, uint8_t reg, uint8_t val );
int32_t read_reg( sdrgg_dev_t *dev, uint8_t reg, uint8_t *val );
int32_t rmw( sdrgg_dev_t *dev, uint8_t reg, uint8_t val, uint8_t mask );

/* Multi-op sessions (single repeater enable for multiple ops) */
int32_t write_batch( sdrgg_dev_t *dev, const uint8_t *reg_val_pairs, uint32_t pair_count );
int32_t read_batch( sdrgg_dev_t *dev, const uint8_t *regs, uint8_t *vals, uint32_t count );

} /* namespace tuner */

/* ========== Core API namespace ========== */

#define SDRGG_GAIN_AUTO  (-1)

namespace sdr {

sdrgg_ctx_t *create( void );
void destroy( sdrgg_ctx_t *ctx );

sdrgg_dev_t *open( sdrgg_ctx_t *ctx, int32_t index );
sdrgg_dev_t *open_path( sdrgg_ctx_t *ctx, const char *path );
void close( sdrgg_dev_t *dev );

int32_t set_frequency( sdrgg_dev_t *dev, uint32_t freq_hz, uint32_t *actual_hz );
int32_t set_sample_rate( sdrgg_dev_t *dev, uint32_t rate_hz, uint32_t *actual_hz );
int32_t set_gain( sdrgg_dev_t *dev, int32_t gain_tenth_db );
int32_t get_gain( sdrgg_dev_t *dev, int32_t *gain_tenth_db );
int32_t set_freq_correction( sdrgg_dev_t *dev, int32_t ppm );
int32_t set_digital_agc( sdrgg_dev_t *dev, bool enable );

int32_t start_stream( sdrgg_dev_t *dev, const sdrgg_stream_cfg_t *cfg, sdrgg_stream_cb_t callback, void *user_ctx );
int32_t stop_stream( sdrgg_dev_t *dev );
int32_t read_sync( sdrgg_dev_t *dev, uint8_t *buf, uint32_t max_bytes, uint32_t timeout_ms );

sdrgg_tuner_type_t get_tuner_type( sdrgg_dev_t *dev );
int32_t get_devinfo( sdrgg_dev_t *dev, sdrgg_devinfo_t *info );
uint32_t get_xtal_freq( sdrgg_dev_t *dev );

} /* namespace sdr */

/* ========== Tuner capability introspection ========== */

/* Gain stage descriptor (one per independent gain element) */
struct gain_stage_info {
  const char *name;          /* e.g. "LNA", "Mixer", "VGA" */
  int32_t     min_tenth_db;  /* minimum gain in 0.1 dB */
  int32_t     max_tenth_db;  /* maximum gain in 0.1 dB */
  uint8_t     num_steps;     /* number of discrete steps (0 = continuous) */
  const int32_t *step_db;   /* per-step incremental gain table (NULL if N/A) */
};

/* Bandwidth option descriptor */
struct bw_option {
  uint32_t  bw_khz;          /* bandwidth in kHz */
};

/* Frequency gap descriptor (for tuners with coverage holes) */
struct freq_gap {
  uint32_t  start_hz;        /* gap start frequency */
  uint32_t  stop_hz;         /* gap stop frequency */
};

/* Mixer architecture type */
typedef enum {
  SDRGG_MIXER_LOW_IF = 0,    /* low-IF with real sampling (R820T, FC001x) */
  SDRGG_MIXER_ZERO_IF,       /* zero-IF with I/Q quadrature (E4000) */
} sdrgg_mixer_arch_t;

/* Complete tuner capability descriptor */
struct tuner_caps {
  sdrgg_tuner_type_t  tuner_type;
  const char         *chip_name;       /* human-readable e.g. "R820T2" */

  /* I2C bus parameters */
  uint16_t  i2c_addr;         /* tuner I2C address on RTL2832U bus */
  uint8_t   i2c_reg_count;    /* number of writable registers */

  /* Frequency range */
  uint32_t  freq_min_hz;
  uint32_t  freq_max_hz;

  /* Frequency gaps (tuners with coverage holes) */
  uint8_t          num_freq_gaps;
  const freq_gap  *freq_gaps;       /* array[num_freq_gaps], NULL if contiguous */

  /* Mixer/IF architecture */
  sdrgg_mixer_arch_t mixer_arch;
  uint32_t  if_freq_hz;       /* default IF frequency (0 for zero-IF) */

  /* Gain architecture */
  uint8_t             num_gain_stages;
  const gain_stage_info *gain_stages;    /* array[num_gain_stages] */
  int32_t             total_gain_min_tenth_db;
  int32_t             total_gain_max_tenth_db;
  bool                has_separate_lna_control;
  bool                has_agc;

  /* IF filter bandwidth options */
  uint8_t             num_bw_options;
  const bw_option    *bw_options;        /* array[num_bw_options], NULL if not configurable */
  uint32_t            default_bw_khz;

  /* PLL characteristics */
  uint32_t  pll_step_hz;    /* finest tuning resolution */
  bool      has_fractional_pll;

  /* Clocking */
  uint32_t  xtal_freq_hz;   /* expected crystal frequency */

  /* Implementation status */
  bool      implemented;     /* true if codec is available in this build */
};

namespace sdr {
int32_t get_tuner_caps( sdrgg_dev_t *dev, const tuner_caps **caps );
} /* namespace sdr */

/* ========== R820T tuner namespace ========== */

namespace r820t {

/* Gain profile: a pre-validated combination of all gain stages */
struct gain_profile {
  int32_t  total_tenth_db;
  uint8_t  lna_index;
  uint8_t  mixer_index;
  uint8_t  vga_index;
};

int32_t init( sdrgg_dev_t *dev );
int32_t standby( sdrgg_dev_t *dev );
int32_t set_freq( sdrgg_dev_t *dev, uint32_t freq_hz, uint32_t if_freq_hz );
int32_t pll_locked( sdrgg_dev_t *dev, bool *locked );
int32_t set_lna_gain( sdrgg_dev_t *dev, int32_t index );
int32_t set_mixer_gain( sdrgg_dev_t *dev, int32_t index );
int32_t set_vga_gain( sdrgg_dev_t *dev, int32_t index );
int32_t set_bandwidth( sdrgg_dev_t *dev, uint32_t bw_khz );
int32_t read_signal( sdrgg_dev_t *dev, uint8_t *lna_idx, uint8_t *mixer_idx );
const gain_profile *select_gain_profile( int32_t target_tenth_db );

extern const int32_t lna_db[16];
extern const int32_t mixer_db[16];

const tuner_caps *get_caps( void );

} /* namespace r820t */

/* ========== FC0012 tuner namespace ========== */

namespace fc0012 {

int32_t detect( sdrgg_dev_t *dev );
int32_t init( sdrgg_dev_t *dev );
int32_t set_freq( sdrgg_dev_t *dev, uint32_t freq_hz );
int32_t set_gain( sdrgg_dev_t *dev, int32_t gain_tenth_db );
int32_t get_gains( const int16_t **gains, int32_t *count );

const tuner_caps *get_caps( void );

} /* namespace fc0012 */

/* ========== FC0013 tuner namespace (not implemented) ========== */

namespace fc0013 {

const tuner_caps *get_caps( void );

} /* namespace fc0013 */

/* ========== FC2580 tuner namespace (not implemented) ========== */

namespace fc2580 {

const tuner_caps *get_caps( void );

} /* namespace fc2580 */

/* ========== E4000 tuner namespace (not implemented) ========== */

namespace e4000 {

const tuner_caps *get_caps( void );

} /* namespace e4000 */

/* ========== Capability query by type (no device needed) ========== */

namespace sdr {
const tuner_caps *get_caps_by_type( sdrgg_tuner_type_t type );
} /* namespace sdr */

/* ========== Device reset namespace ========== */

/*
 * Reset levels (from lightest to heaviest):
 *
 *   LEVEL 1 - reset_demod: Soft-reset the RTL2832U digital core via register
 *             write (page 1, reg 0x01, bit 2). Resets DSP pipeline without
 *             touching tuner or USB. Fastest recovery for DSP glitches.
 *
 *   LEVEL 2 - reset_tuner: Re-initialize all tuner registers from the shadow
 *             register file. Does NOT reset the demodulator. Fixes corrupted
 *             I2C/tuner state without USB disturbance.
 *
 *   LEVEL 3 - reset_usb: Issue ioctl(USBDEVFS_RESET) on the device fd.
 *             Resets USB protocol state. Device re-enumerates but does NOT
 *             power-cycle. After this, device must be re-opened.
 *
 *   LEVEL 4 - reset_power: Disable and re-enable USB port power via sysfs
 *             authorized attribute. True cold reset equivalent to physical
 *             unplug/replug. After this, device must be re-opened.
 *
 * Return: SDRGG_OK on success, or a negative SDRGG_ERR_* code.
 */

namespace reset {

/* Level 1: RTL2832U demodulator soft reset (DSP pipeline only) */
int32_t reset_demod( sdrgg_dev_t *dev );

/* Level 2: Tuner register re-initialization from shadow */
int32_t reset_tuner( sdrgg_dev_t *dev );

/* Level 3: USB device reset (ioctl USBDEVFS_RESET) */
int32_t reset_usb( sdrgg_dev_t *dev );

/* Level 4: USB port power cycle via sysfs (true cold reset)
 * usb_path: e.g. "1-1.1.1" (from devinfo.path or lsusb -t)
 * If NULL, attempts to resolve from the open device fd. */
int32_t reset_power( sdrgg_dev_t *dev, const char *usb_path );

/* Convenience: full reset sequence (levels 1+2+3) without power cycle */
int32_t reset_full( sdrgg_dev_t *dev );

} /* namespace reset */

#endif /* __cplusplus */

#endif /* SDRGG_H */
