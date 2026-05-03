/*
* sdrgg_internal.h - Private internals for libsdrgg
*
* Decomposes device state into isolated domain types:
*   device_identity    - immutable properties established at attach
*   baseband_state     - RTL2832U demodulator runtime
*   tuner_shadow       - tuner register file mirror (intended state)
*   tuning_context     - RF/sampling with requested/committed tracking
*   stream_engine      - async streaming machinery
*   stream_pipeline    - buffer arena + submission/completion tracking
*
* Not part of the public API.
*
* License: MIT
*/

#ifndef SDRGG_INTERNAL_H
#define SDRGG_INTERNAL_H

#include "sdrgg.h"
#include <atomic>
#include <pthread.h>
#include <linux/usbdevice_fs.h>

/* ======================================================================
*  USB protocol constants
* ====================================================================== */

#define SDRGG_USB_VID_REALTEK       0x0BDA
#define SDRGG_USB_PID_2832          0x2832
#define SDRGG_USB_PID_2838          0x2838

#define SDRGG_USB_EPA               0x81
#define SDRGG_USB_TIMEOUT_MS        1000

#define SDRGG_CTRL_OUT              0x40
#define SDRGG_CTRL_IN               0xC0

/* ======================================================================
*  RTL2832U register address constants
*  Grouped by functional subsystem.
* ====================================================================== */

/* Block identifiers for transport addressing */
#define SDRGG_BLOCK_DEMOD           0
#define SDRGG_BLOCK_USB             1
#define SDRGG_BLOCK_SYS             2
#define SDRGG_BLOCK_TUNER           3

/* Block base addresses */
#define SDRGG_DEMOD_BASE            0x0000
#define SDRGG_USB_BASE              0x0100
#define SDRGG_SYS_BASE              0x0200

/* System subsystem */
#define SDRGG_SYS_DEMOD_CTL        0x3000
#define SDRGG_SYS_DEMOD_CTL_1     0x300B
#define SDRGG_SYS_GPO              0x3001
#define SDRGG_SYS_GPI              0x3002
#define SDRGG_SYS_GPOE             0x3003
#define SDRGG_SYS_GPD              0x3004

/* USB subsystem */
#define SDRGG_USB_SYSCTL           0x2000
#define SDRGG_USB_EPA_CFG          0x2144
#define SDRGG_USB_EPA_CTL          0x2148
#define SDRGG_USB_EPA_MAXPKT       0x2158
#define SDRGG_USB_EPA_FIFO_CFG     0x2160

/* I2C repeater gate values */
#define SDRGG_I2C_REPEATER_ON      0x18
#define SDRGG_I2C_REPEATER_OFF     0x10

/* ======================================================================
*  Tuner hardware constants
* ====================================================================== */

#define SDRGG_R820T_I2C_ADDR       0x34
#define SDRGG_R820T_I2C_ADDR_W     0x34
#define SDRGG_R820T_IF_FREQ        3570000
#define SDRGG_R820T_NUM_REGS       27
#define SDRGG_R820T_REG_START      0x05

#define SDRGG_XTAL_FREQ            28800000

/* ======================================================================
*  Streaming resource limits
* ====================================================================== */

#define SDRGG_STREAM_BUF_COUNT     32
#define SDRGG_STREAM_BUF_SIZE      (16 * 1024)
#define SDRGG_MIN_BUF_SIZE         512
#define SDRGG_MAX_BUF_SIZE         (256 * 1024)

#define SDRGG_MAX_DEVICES          8

/* ======================================================================
*  URB descriptor (tracks one in-flight USB bulk transfer)
* ====================================================================== */

typedef struct sdrgg_urb {
  struct usbdevfs_urb urb;
  sdrgg_dev_t *dev;
  uint8_t *buffer;
  uint32_t buf_size;
  uint32_t index;
  bool submitted;
} sdrgg_urb_t;

/* ======================================================================
*  Stream pipeline (buffer arena + submission/completion/drop accounting)
* ====================================================================== */

struct stream_pipeline {
  sdrgg_urb_t *arena;         /* buffer arena: array of URB descriptors */
  uint32_t     arena_depth;   /* number of URBs in the arena */
  uint32_t     segment_size;  /* individual buffer size */
  uint32_t     submitted;     /* URBs currently in-flight */
  uint32_t     completed;     /* total successfully reaped */
  uint32_t     dropped;       /* total failed/cancelled reaps */
  bool         backpressure;  /* true when all slots in-flight */
};

/* ======================================================================
*  Decomposed device state types
* ====================================================================== */

/* Immutable properties fixed after device attach */
struct device_identity {
  sdrgg_ctx_t *ctx;
  int32_t fd;
  int32_t slot_index;
  sdrgg_devinfo_t info;
  int32_t bulk_endpoint;
  sdrgg_tuner_type_t tuner_class;
  uint16_t tuner_bus_addr;
};

/* RTL2832U baseband runtime configuration */
struct baseband_state {
  uint32_t oscillator_hz;
  int32_t  freq_offset_ppm;
};

/* Tuner register file mirror (intended hardware state) */
struct tuner_shadow {
  uint8_t r820t_file[SDRGG_R820T_NUM_REGS];   /* last-written register values */
};

/* Tuner observed state (readback verification snapshot) */
struct tuner_observed {
  uint8_t  last_readback[SDRGG_R820T_NUM_REGS]; /* last verified readback */
  bool     readback_valid;                       /* true if snapshot is current */
  uint32_t readback_age_us;                      /* microseconds since last verify */
};

/* RF and sampling parameters with request/commit lifecycle */
struct tuning_context {
  /* Committed state (successfully applied to hardware) */
  uint32_t center_freq_hz;
  uint32_t sampling_rate_hz;
  uint32_t if_offset_hz;
  int32_t  gain_policy;

  /* Requested state (written before hardware attempt) */
  uint32_t requested_freq_hz;
  uint32_t requested_rate_hz;

  /* Commit status (tracks last operation outcome) */
  int32_t  last_freq_result;     /* SDRGG_OK or error from last freq commit */
  int32_t  last_rate_result;     /* SDRGG_OK or error from last rate commit */
};

/* Async streaming engine state */
struct stream_engine {
  std::atomic<bool> active;
  sdrgg_stream_cb_t callback;
  void *user_data;
  uint32_t sequence;
  std::atomic<bool> cancel_requested;
};

/* ======================================================================
*  Top-level device handle (composes domain types)
* ====================================================================== */

struct sdrgg_dev {
  device_identity    identity;
  baseband_state     baseband;
  tuner_shadow       shadow;
  tuner_observed     observed;
  tuning_context     tuning;
  stream_engine      stream;
  stream_pipeline    pipeline;
  pthread_mutex_t    lock;
};

/* ======================================================================
*  Context (multi-device coordinator + event loop)
* ====================================================================== */

struct sdrgg_ctx {
  sdrgg_dev_t *devices[SDRGG_MAX_DEVICES];
  int32_t dev_count;
  pthread_mutex_t lock;

  int32_t epoll_fd;
  int32_t event_pipe[2];
  pthread_t event_thread;
  std::atomic<bool> event_running;
  std::atomic<int32_t> streaming_count;
};

/* ======================================================================
*  Internal function declarations: USB transport
* ====================================================================== */

namespace usb {

int32_t control_write( sdrgg_dev_t *dev, uint16_t value, uint16_t index, const uint8_t *data, uint16_t len );
int32_t control_read( sdrgg_dev_t *dev, uint16_t value, uint16_t index, uint8_t *data, uint16_t len );
int32_t bulk_read( sdrgg_dev_t *dev, uint8_t *buf, uint32_t len, uint32_t timeout_ms, uint32_t *actual );
int32_t claim( sdrgg_dev_t *dev );
int32_t release( sdrgg_dev_t *dev );

int32_t urb_alloc( sdrgg_dev_t *dev, uint32_t count, uint32_t buf_size );
void urb_free( sdrgg_dev_t *dev );
int32_t urb_submit( sdrgg_dev_t *dev, sdrgg_urb_t *u );
int32_t urb_submit_all( sdrgg_dev_t *dev );
int32_t urb_cancel_all( sdrgg_dev_t *dev );
sdrgg_urb_t *urb_reap( sdrgg_dev_t *dev );

int32_t event_loop_start( sdrgg_ctx_t *ctx );
void event_loop_stop( sdrgg_ctx_t *ctx );
int32_t event_loop_add_dev( sdrgg_ctx_t *ctx, sdrgg_dev_t *dev );
int32_t event_loop_remove_dev( sdrgg_ctx_t *ctx, sdrgg_dev_t *dev );

int32_t enumerate( sdrgg_ctx_t *ctx, sdrgg_devinfo_t *devs, int32_t max_devs );

uint8_t bitrev8( uint8_t byte );
uint64_t time_us( void );

} /* namespace usb */

/* ======================================================================
*  Internal function declarations: RTL2832U baseband engine
* ====================================================================== */

namespace rtl {

int32_t init( sdrgg_dev_t *dev );
int32_t deinit( sdrgg_dev_t *dev );
int32_t set_sample_rate( sdrgg_dev_t *dev, uint32_t rate_hz );
int32_t set_if_freq( sdrgg_dev_t *dev, uint32_t if_freq_hz );
int32_t enable_i2c_repeater( sdrgg_dev_t *dev, bool enable );
int32_t start_bulk( sdrgg_dev_t *dev );
int32_t stop_bulk( sdrgg_dev_t *dev );
int32_t configure_r820t( sdrgg_dev_t *dev );
void set_gpio_output( sdrgg_dev_t *dev, uint8_t gpio );
void set_gpio_bit( sdrgg_dev_t *dev, uint8_t gpio, int32_t val );

} /* namespace rtl */

/* ======================================================================
*  Internal function declarations: Tuner codecs
* ====================================================================== */

namespace r820t {
  int32_t detect( sdrgg_dev_t *dev );
  int32_t set_pll( sdrgg_dev_t *dev, uint32_t freq_hz );
  int32_t set_mux( sdrgg_dev_t *dev, uint32_t freq_hz );
}

#endif /* SDRGG_INTERNAL_H */
