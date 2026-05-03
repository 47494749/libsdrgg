/*
* sdrgg_rtl.cpp - RTL2832U baseband engine
*
* Lifecycle-phase architecture:
*   Address descriptors  - typed register targeting
*   Boot specification   - declarative phase-based initialization
*   Coefficient service  - FIR filter deployment
*   Scoped bus sessions  - I2C repeater with RAII-like scoping
*   Configuration API    - sample rate, IF, streaming control
*
* License: MIT
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#include "sdrgg_internal.h"

/* ======================================================================
*  Address descriptors
*
*  Each register access is targeted through a typed descriptor
*  that carries the address space, page/bank info, and width.
*  This replaces the raw enum-based space selector.
* ====================================================================== */

namespace {

enum class reg_domain : uint8_t {
  USB_FABRIC,       /* USB block registers (0x2000+) */
  SYSTEM_FABRIC,    /* System block registers (0x3000+) */
  DEMOD_INTERNAL,   /* Demodulator internal (paged) */
};

struct reg_descriptor {
  reg_domain domain;
  uint8_t    page;        /* demod page (0 or 1) */
  uint16_t   offset;      /* register offset or address */
  uint16_t   payload;     /* value to write */
  uint8_t    width;       /* 1 or 2 bytes */
};

/* ---- Descriptor sequence (stack-allocated) ---- */

static constexpr uint32_t DESC_SEQ_MAX = 32;

struct desc_sequence {
  reg_descriptor items[DESC_SEQ_MAX];
  uint32_t count;
};

static inline void dseq_clear( desc_sequence *ds ) {
  ds->count = 0;
}

static inline bool dseq_add_fabric( desc_sequence *ds, reg_domain domain, uint16_t offset, uint16_t payload, uint8_t width ) {
  if( ds->count >= DESC_SEQ_MAX ) return false;
  ds->items[ds->count] = { domain, 0, offset, payload, width };
  ds->count++;
  return true;
}

static inline bool dseq_add_demod( desc_sequence *ds, uint8_t page, uint8_t reg, uint16_t payload, uint8_t width ) {
  if( ds->count >= DESC_SEQ_MAX ) return false;
  ds->items[ds->count] = { reg_domain::DEMOD_INTERNAL, page, reg, payload, width };
  ds->count++;
  return true;
}

/* ======================================================================
*  TRANSPORT LAYER
*  Raw register access via USB control transfers.
* ====================================================================== */

int32_t submit_block_write( sdrgg_dev_t *dev, uint8_t block, uint16_t addr, uint16_t val, uint8_t len ) {
  uint8_t data[2];
  uint16_t wIndex = ( (uint16_t)block << 8 ) | 0x10;

  if( len == 1 ) {
    data[0] = val & 0xFF;
  } else {
    data[0] = ( val >> 8 ) & 0xFF;
    data[1] = val & 0xFF;
  }

  return usb::control_write( dev, addr, wIndex, data, len );
}

int32_t submit_block_read( sdrgg_dev_t *dev, uint8_t block, uint16_t addr, uint8_t *val ) {
  uint16_t wIndex = ( (uint16_t)block << 8 );
  return usb::control_read( dev, addr, wIndex, val, 1 );
}

int32_t submit_demod_write( sdrgg_dev_t *dev, uint8_t page, uint8_t reg, uint16_t val, uint8_t len ) {
  uint8_t data[2];
  uint16_t wIndex = 0x10 | page;
  uint16_t wValue = ( (uint16_t)reg << 8 ) | 0x20;

  if( len == 1 ) {
    data[0] = val & 0xFF;
  } else {
    data[0] = ( val >> 8 ) & 0xFF;
    data[1] = val & 0xFF;
  }

  int32_t rc = usb::control_write( dev, wValue, wIndex, data, len );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Write confirmation readback (protocol requirement) */
  uint8_t dummy;
  usb::control_read( dev, 0x0120, 0x000a, &dummy, 1 );

  return SDRGG_OK;
}

int32_t submit_demod_read( sdrgg_dev_t *dev, uint8_t page, uint8_t reg, uint8_t *val ) {
  uint16_t wIndex = page;
  uint16_t wValue = ( (uint16_t)reg << 8 ) | 0x20;
  return usb::control_read( dev, wValue, wIndex, val, 1 );
}

/* ======================================================================
*  Descriptor sequence executor
*  Applies a descriptor sequence to hardware via the transport layer.
* ====================================================================== */

static int32_t apply_descriptors( sdrgg_dev_t *dev, const desc_sequence *ds ) {
  for( uint32_t i = 0; i < ds->count; i++ ) {
    const reg_descriptor *rd = &ds->items[i];
    int32_t rc;

    switch( rd->domain ) {
    case reg_domain::USB_FABRIC:
      rc = submit_block_write( dev, SDRGG_BLOCK_USB, rd->offset, rd->payload, rd->width );
      break;
    case reg_domain::SYSTEM_FABRIC:
      rc = submit_block_write( dev, SDRGG_BLOCK_SYS, rd->offset, rd->payload, rd->width );
      break;
    case reg_domain::DEMOD_INTERNAL:
      rc = submit_demod_write( dev, rd->page, (uint8_t)rd->offset, rd->payload, rd->width );
      break;
    default:
      rc = SDRGG_ERR_PARAM;
      break;
    }

    if( rc != SDRGG_OK ) {
      return rc;
    }
  }
  return SDRGG_OK;
}

} /* anonymous namespace */

/* ======================================================================
*  PUBLIC DEMOD REGISTER API (exported via sdrgg.h)
* ====================================================================== */

int32_t demod::write( sdrgg_dev_t *dev, uint8_t block, uint16_t reg, uint8_t val ) {
  switch( block ) {
  case SDRGG_BLOCK_DEMOD:
    return submit_demod_write( dev, ( reg >> 8 ) & 0xFF, reg & 0xFF, val, 1 );
  case SDRGG_BLOCK_USB:
  case SDRGG_BLOCK_SYS:
    return submit_block_write( dev, block, reg, val, 1 );
  default:
    return SDRGG_ERR_PARAM;
  }
}

int32_t demod::read( sdrgg_dev_t *dev, uint8_t block, uint16_t reg, uint8_t *val ) {
  switch( block ) {
  case SDRGG_BLOCK_DEMOD:
    return submit_demod_read( dev, ( reg >> 8 ) & 0xFF, reg & 0xFF, val );
  case SDRGG_BLOCK_USB:
  case SDRGG_BLOCK_SYS:
    return submit_block_read( dev, block, reg, val );
  default:
    return SDRGG_ERR_PARAM;
  }
}

int32_t demod::write_bulk( sdrgg_dev_t *dev, uint8_t block, uint16_t reg, const uint8_t *data, uint16_t len ) {
  switch( block ) {
  case SDRGG_BLOCK_DEMOD: {
    uint8_t page = ( reg >> 8 ) & 0xFF;
    uint8_t regoff = reg & 0xFF;
    for( uint16_t i = 0; i < len; i++ ) {
      int32_t rc = submit_demod_write( dev, page, regoff + i, data[i], 1 );
      if( rc != SDRGG_OK ) {
        return rc;
      }
    }
    return SDRGG_OK;
  }
  case SDRGG_BLOCK_USB: {
    uint16_t wIndex = 0x0110;
    return usb::control_write( dev, reg, wIndex, data, len );
  }
  case SDRGG_BLOCK_SYS: {
    uint16_t wIndex = 0x0210;
    return usb::control_write( dev, reg, wIndex, data, len );
  }
  default:
    return SDRGG_ERR_PARAM;
  }
}

int32_t demod::read_bulk( sdrgg_dev_t *dev, uint8_t block, uint16_t reg, uint8_t *data, uint16_t len ) {
  switch( block ) {
  case SDRGG_BLOCK_DEMOD: {
    uint8_t page = ( reg >> 8 ) & 0xFF;
    uint8_t regoff = reg & 0xFF;
    for( uint16_t i = 0; i < len; i++ ) {
      int32_t rc = submit_demod_read( dev, page, regoff + i, &data[i] );
      if( rc != SDRGG_OK ) {
        return rc;
      }
    }
    return SDRGG_OK;
  }
  case SDRGG_BLOCK_USB: {
    uint16_t wIndex = 0x0100;
    return usb::control_read( dev, reg, wIndex, data, len );
  }
  case SDRGG_BLOCK_SYS: {
    uint16_t wIndex = 0x0200;
    return usb::control_read( dev, reg, wIndex, data, len );
  }
  default:
    return SDRGG_ERR_PARAM;
  }
}

/* ======================================================================
*  SCOPED I2C BUS SESSION
*
*  Tuner register access through the I2C repeater gate.
*  Two access modes:
*
*  1) Single-op: The repeater is opened and closed around each
*     individual register access. Simplest and safest.
*
*  2) Multi-op session: The repeater stays open for a batch of
*     operations. This reduces USB overhead when performing
*     multiple sequential tuner register writes.
*     An i2c_session object manages the lifetime.
* ====================================================================== */

namespace {

struct i2c_session {
  sdrgg_dev_t *dev;
  bool active;
  uint32_t op_count;
};

static int32_t i2c_session_begin( i2c_session *s, sdrgg_dev_t *dev ) {
  s->dev = dev;
  s->op_count = 0;
  int32_t rc = rtl::enable_i2c_repeater( dev, true );
  s->active = ( rc == SDRGG_OK );
  return rc;
}

static int32_t i2c_session_end( i2c_session *s ) {
  if( !s->active ) return SDRGG_OK;
  s->active = false;
  return rtl::enable_i2c_repeater( s->dev, false );
}

static int32_t i2c_session_write( i2c_session *s, uint8_t reg, uint8_t val ) {
  if( !s->active ) return SDRGG_ERR_IO;
  uint8_t buf[2] = { reg, val };
  uint16_t addr = s->dev->identity.tuner_bus_addr;
  int32_t rc = usb::control_write( s->dev, addr, 0x0610, buf, 2 );
  if( rc == SDRGG_OK ) {
    /* Maintain shadow if R820T */
    if( ( s->dev->identity.tuner_class == SDRGG_TUNER_R820T || s->dev->identity.tuner_class == SDRGG_TUNER_R820T2 ) &&
        reg >= SDRGG_R820T_REG_START && reg < SDRGG_R820T_REG_START + SDRGG_R820T_NUM_REGS ) {
      s->dev->shadow.r820t_file[reg - SDRGG_R820T_REG_START] = val;
    }
    s->op_count++;
  }
  return rc;
}

static int32_t i2c_session_read( i2c_session *s, uint8_t reg, uint8_t *data, uint8_t len ) {
  if( !s->active ) return SDRGG_ERR_IO;
  uint16_t addr = s->dev->identity.tuner_bus_addr;
  uint8_t raw[32];
  uint32_t total = (uint32_t)reg + (uint32_t)len;
  if( total > 32 ) return SDRGG_ERR_PARAM;

  int32_t rc = usb::control_read( s->dev, addr, 0x0600, raw, total );
  if( rc != SDRGG_OK ) return rc;

  /* Bit-reversal for R820T */
  if( s->dev->identity.tuner_class == SDRGG_TUNER_R820T || s->dev->identity.tuner_class == SDRGG_TUNER_R820T2 ) {
    for( uint8_t i = 0; i < len; i++ ) {
      data[i] = usb::bitrev8( raw[reg + i] );
    }
  } else {
    for( uint8_t i = 0; i < len; i++ ) {
      data[i] = raw[reg + i];
    }
  }
  s->op_count++;
  return SDRGG_OK;
}

} /* anonymous namespace */

int32_t rtl::enable_i2c_repeater( sdrgg_dev_t *dev, bool enable ) {
  uint8_t val = enable ? SDRGG_I2C_REPEATER_ON : SDRGG_I2C_REPEATER_OFF;
  return submit_demod_write( dev, 1, 0x01, val, 1 );
}

/* Scoped tuner write: open session → transfer → close session */
int32_t tuner::write( sdrgg_dev_t *dev, const uint8_t *data, uint8_t len ) {
  int32_t rc = rtl::enable_i2c_repeater( dev, true );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  uint16_t addr = dev->identity.tuner_bus_addr;
  rc = usb::control_write( dev, addr, 0x0610, data, len );

  rtl::enable_i2c_repeater( dev, false );
  return rc;
}

/* Scoped tuner read with bit-reversal for R820T */
int32_t tuner::read( sdrgg_dev_t *dev, uint8_t reg, uint8_t *data, uint8_t len ) {
  int32_t rc = rtl::enable_i2c_repeater( dev, true );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  uint16_t addr = dev->identity.tuner_bus_addr;
  uint8_t raw[32];
  uint32_t total = (uint32_t)reg + (uint32_t)len;
  if( total > 32 ) {
    rtl::enable_i2c_repeater( dev, false );
    return SDRGG_ERR_PARAM;
  }

  rc = usb::control_read( dev, addr, 0x0600, raw, total );

  rtl::enable_i2c_repeater( dev, false );

  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* R820T readback is bit-reversed; other tuners pass through */
  if( dev->identity.tuner_class == SDRGG_TUNER_R820T || dev->identity.tuner_class == SDRGG_TUNER_R820T2 ) {
    for( uint8_t i = 0; i < len; i++ ) {
      data[i] = usb::bitrev8( raw[reg + i] );
    }
  } else {
    for( uint8_t i = 0; i < len; i++ ) {
      data[i] = raw[reg + i];
    }
  }

  return SDRGG_OK;
}

/* Scoped single register write + shadow maintenance */
int32_t tuner::write_reg( sdrgg_dev_t *dev, uint8_t reg, uint8_t val ) {
  uint8_t buf[2] = { reg, val };
  int32_t rc = tuner::write( dev, buf, 2 );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Maintain R820T shadow register file */
  if( ( dev->identity.tuner_class == SDRGG_TUNER_R820T || dev->identity.tuner_class == SDRGG_TUNER_R820T2 ) &&
      reg >= SDRGG_R820T_REG_START && reg < SDRGG_R820T_REG_START + SDRGG_R820T_NUM_REGS ) {
    dev->shadow.r820t_file[reg - SDRGG_R820T_REG_START] = val;
  }
  return SDRGG_OK;
}

/* Scoped single register read */
int32_t tuner::read_reg( sdrgg_dev_t *dev, uint8_t reg, uint8_t *val ) {
  return tuner::read( dev, reg, val, 1 );
}

/* Scoped read-modify-write with shadow support */
int32_t tuner::rmw( sdrgg_dev_t *dev, uint8_t reg, uint8_t val, uint8_t mask ) {
  uint8_t cur;

  /* Use shadow for R820T; actual read for others */
  if( ( dev->identity.tuner_class == SDRGG_TUNER_R820T || dev->identity.tuner_class == SDRGG_TUNER_R820T2 ) &&
      reg >= SDRGG_R820T_REG_START && reg < SDRGG_R820T_REG_START + SDRGG_R820T_NUM_REGS ) {
    cur = dev->shadow.r820t_file[reg - SDRGG_R820T_REG_START];
  } else {
    int32_t rc = tuner::read_reg( dev, reg, &cur );
    if( rc != SDRGG_OK ) {
      return rc;
    }
  }

  uint8_t combined = ( cur & ~mask ) | ( val & mask );
  return tuner::write_reg( dev, reg, combined );
}

/* Multi-op session: write multiple registers under single repeater enable */
int32_t tuner::write_batch( sdrgg_dev_t *dev, const uint8_t *reg_val_pairs, uint32_t pair_count ) {
  i2c_session sess;
  int32_t rc = i2c_session_begin( &sess, dev );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  for( uint32_t i = 0; i < pair_count; i++ ) {
    rc = i2c_session_write( &sess, reg_val_pairs[i * 2], reg_val_pairs[i * 2 + 1] );
    if( rc != SDRGG_OK ) {
      i2c_session_end( &sess );
      return rc;
    }
  }

  return i2c_session_end( &sess );
}

/* Multi-op session: read multiple registers under single repeater enable */
int32_t tuner::read_batch( sdrgg_dev_t *dev, const uint8_t *regs, uint8_t *vals, uint32_t count ) {
  i2c_session sess;
  int32_t rc = i2c_session_begin( &sess, dev );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  for( uint32_t i = 0; i < count; i++ ) {
    rc = i2c_session_read( &sess, regs[i], &vals[i], 1 );
    if( rc != SDRGG_OK ) {
      i2c_session_end( &sess );
      return rc;
    }
  }

  return i2c_session_end( &sess );
}

/* ======================================================================
*  GPIO FABRIC (band switching, tuner reset)
* ====================================================================== */

void rtl::set_gpio_output( sdrgg_dev_t *dev, uint8_t gpio ) {
  uint8_t r, mask = 1 << gpio;

  submit_block_read( dev, SDRGG_BLOCK_SYS, SDRGG_SYS_GPD, &r );
  submit_block_write( dev, SDRGG_BLOCK_SYS, SDRGG_SYS_GPD, r & ~mask, 1 );
  submit_block_read( dev, SDRGG_BLOCK_SYS, SDRGG_SYS_GPOE, &r );
  submit_block_write( dev, SDRGG_BLOCK_SYS, SDRGG_SYS_GPOE, r | mask, 1 );
}

void rtl::set_gpio_bit( sdrgg_dev_t *dev, uint8_t gpio, int32_t val ) {
  uint8_t r, mask = 1 << gpio;

  submit_block_read( dev, SDRGG_BLOCK_SYS, SDRGG_SYS_GPO, &r );
  r = val ? ( r | mask ) : ( r & ~mask );
  submit_block_write( dev, SDRGG_BLOCK_SYS, SDRGG_SYS_GPO, r, 1 );
}

/* ======================================================================
*  COEFFICIENT DEPLOYMENT SERVICE
*
*  Deploys FIR filter coefficients to the demodulator.
*  The 32-tap symmetric low-pass filter is specified by its
*  first 16 taps; the deployment service handles the packing
*  format (8-bit for taps 0-7, 12-bit paired for taps 8-15).
* ====================================================================== */

namespace {

static const int16_t filter_kernel[16] = {
  -54, -36, -41, -40, -32, -14, 14, 53,
  101, 156, 215, 273, 327, 372, 404, 421
};

int32_t deploy_filter_coefficients( sdrgg_dev_t *dev ) {
  uint8_t packed[20];

  /* Pack taps 0..7: direct 8-bit signed representation */
  for( int32_t k = 0; k < 8; k++ ) {
    packed[k] = (uint8_t)filter_kernel[k];
  }

  /* Pack taps 8..15: 12-bit signed, interleaved in pairs */
  for( int32_t k = 0; k < 8; k += 2 ) {
    int32_t a = filter_kernel[8 + k];
    int32_t b = filter_kernel[8 + k + 1];
    packed[8 + k * 3 / 2]     = (uint8_t)( a >> 4 );
    packed[8 + k * 3 / 2 + 1] = (uint8_t)( ( a << 4 ) | ( ( b >> 8 ) & 0x0F ) );
    packed[8 + k * 3 / 2 + 2] = (uint8_t)( b );
  }

  /* Deploy packed coefficients to demod page 1 registers 0x1C..0x2F */
  for( int32_t k = 0; k < 20; k++ ) {
    int32_t rc = submit_demod_write( dev, 1, 0x1C + k, packed[k], 1 );
    if( rc != SDRGG_OK ) {
      return rc;
    }
  }

  return SDRGG_OK;
}

/* ======================================================================
*  BOOT SPECIFICATION — Declarative lifecycle phases
*
*  Each boot phase is described declaratively as a descriptor
*  sequence. The boot orchestrator applies phases in order,
*  with prerequisite resolution (retry on USB connectivity failure).
* ====================================================================== */

enum class boot_phase : uint8_t {
  USB_ATTACH,           /* USB connectivity verification */
  ENDPOINT_SETUP,       /* Bulk endpoint configuration */
  DEMOD_POWERUP,        /* Demodulator power-on */
  DDC_INITIALIZE,       /* Digital down-converter reset + clear */
  FILTER_DEPLOY,        /* FIR coefficient deployment (special) */
  SIGNAL_MODE,          /* SDR signal path configuration */
};

void spec_usb_attach( desc_sequence *ds ) {
  dseq_clear( ds );
  dseq_add_fabric( ds, reg_domain::USB_FABRIC, SDRGG_USB_SYSCTL, 0x09, 1 );
}

void spec_endpoint_setup( desc_sequence *ds ) {
  dseq_clear( ds );
  dseq_add_fabric( ds, reg_domain::USB_FABRIC, SDRGG_USB_EPA_MAXPKT, 0x0002, 2 );
  dseq_add_fabric( ds, reg_domain::USB_FABRIC, SDRGG_USB_EPA_CTL, 0x1002, 2 );
}

void spec_demod_powerup( desc_sequence *ds ) {
  dseq_clear( ds );
  dseq_add_fabric( ds, reg_domain::SYSTEM_FABRIC, SDRGG_SYS_DEMOD_CTL_1, 0x22, 1 );
  dseq_add_fabric( ds, reg_domain::SYSTEM_FABRIC, SDRGG_SYS_DEMOD_CTL, 0xE8, 1 );
}

void spec_ddc_initialize( desc_sequence *ds ) {
  dseq_clear( ds );
  dseq_add_demod( ds, 1, 0x01, 0x14, 1 );   /* Assert reset */
  dseq_add_demod( ds, 1, 0x01, 0x10, 1 );   /* Release reset */
  dseq_add_demod( ds, 1, 0x15, 0x00, 1 );   /* Spectrum control */
  dseq_add_demod( ds, 1, 0x16, 0x0000, 2 ); /* DDC shift clear */
  /* Clear individual DDC shift registers */
  for( uint8_t k = 0; k < 6; k++ ) {
    dseq_add_demod( ds, 1, 0x16 + k, 0x00, 1 );
  }
}

void spec_signal_mode( desc_sequence *ds ) {
  dseq_clear( ds );
  dseq_add_demod( ds, 0, 0x19, 0x05, 1 );   /* SDR mode select */
  dseq_add_demod( ds, 1, 0x93, 0xF0, 1 );   /* FSM state hold A */
  dseq_add_demod( ds, 1, 0x94, 0x0F, 1 );   /* FSM state hold B */
  dseq_add_demod( ds, 1, 0x11, 0x00, 1 );   /* DAGC disabled */
  dseq_add_demod( ds, 1, 0x04, 0x00, 1 );   /* RF/IF AGC disabled */
  dseq_add_demod( ds, 0, 0x61, 0x60, 1 );   /* PID filter bypass */
  dseq_add_demod( ds, 0, 0x06, 0x80, 1 );   /* ADC path select */
  dseq_add_demod( ds, 1, 0xB1, 0x1B, 1 );   /* Zero-IF + DC cancel */
  dseq_add_demod( ds, 0, 0x0D, 0x83, 1 );   /* Debug clock disabled */
}

/* R820T-specific IF mode configuration */
void spec_r820t_if_mode( desc_sequence *ds ) {
  dseq_clear( ds );
  dseq_add_demod( ds, 1, 0xB1, 0x1A, 1 );   /* Low-IF mode */
  dseq_add_demod( ds, 0, 0x08, 0x4D, 1 );   /* In-phase ADC only */
  dseq_add_demod( ds, 1, 0x15, 0x01, 1 );   /* Spectrum inversion */
}

} /* anonymous namespace */

/* ======================================================================
*  BOOT ORCHESTRATOR
*
*  Applies all lifecycle phases in sequence.
*  Phase 0 (USB_ATTACH) has prerequisite resolution: if it fails,
*  the orchestrator performs a USB device reset and retries.
* ====================================================================== */

int32_t rtl::init( sdrgg_dev_t *dev ) {
  int32_t rc;
  desc_sequence phase_spec;

  /* Phase 0: USB connectivity with prerequisite resolution */
  spec_usb_attach( &phase_spec );
  rc = apply_descriptors( dev, &phase_spec );
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "sdrgg: USB connectivity failed, resetting device...\n" );
    ioctl( dev->identity.fd, USBDEVFS_RESET, NULL );
    int32_t iface = 0;
    ioctl( dev->identity.fd, USBDEVFS_CLAIMINTERFACE, &iface );
    usleep( 100000 );
    rc = apply_descriptors( dev, &phase_spec );
    if( rc != SDRGG_OK ) {
      return rc;
    }
  }

  /* Phase 1: Endpoint configuration */
  spec_endpoint_setup( &phase_spec );
  rc = apply_descriptors( dev, &phase_spec );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Phase 2: Demodulator power-on */
  spec_demod_powerup( &phase_spec );
  rc = apply_descriptors( dev, &phase_spec );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Phase 3: DDC initialization */
  spec_ddc_initialize( &phase_spec );
  rc = apply_descriptors( dev, &phase_spec );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Phase 4: Filter coefficient deployment (special path) */
  rc = deploy_filter_coefficients( dev );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Phase 5: SDR signal mode */
  spec_signal_mode( &phase_spec );
  rc = apply_descriptors( dev, &phase_spec );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  dev->baseband.oscillator_hz = SDRGG_XTAL_FREQ;
  return SDRGG_OK;
}

/* R820T-specific IF mode activation */
int32_t rtl::configure_r820t( sdrgg_dev_t *dev ) {
  desc_sequence phase_spec;
  spec_r820t_if_mode( &phase_spec );

  int32_t rc = apply_descriptors( dev, &phase_spec );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Program IF center frequency */
  rc = rtl::set_if_freq( dev, SDRGG_R820T_IF_FREQ );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  return SDRGG_OK;
}

/* Shutdown: halt streaming, demod power-down */
int32_t rtl::deinit( sdrgg_dev_t *dev ) {
  rtl::stop_bulk( dev );
  submit_block_write( dev, SDRGG_BLOCK_SYS, SDRGG_SYS_DEMOD_CTL, 0x20, 1 );
  return SDRGG_OK;
}

/* ======================================================================
*  SAMPLE RATE CONFIGURATION
*  Computes resampler ratio and programs DDC.
* ====================================================================== */

int32_t rtl::set_sample_rate( sdrgg_dev_t *dev, uint32_t rate_hz ) {
  if( rate_hz < 225000 || rate_hz > 3200000 ) {
    return SDRGG_ERR_PARAM;
  }

  uint32_t xtal = dev->baseband.oscillator_hz;

  /* Apply PPM correction to crystal reference */
  if( dev->baseband.freq_offset_ppm != 0 ) {
    int64_t corrected = (int64_t)xtal * ( 1000000 + dev->baseband.freq_offset_ppm );
    xtal = (uint32_t)( corrected / 1000000 );
  }

  /* Compute 28-bit resampler ratio (Q22 fixed-point) */
  uint32_t ratio = (uint32_t)( ( (uint64_t)xtal * ( 1ULL << 22 ) ) / rate_hz );
  ratio &= 0x0FFFFFFC;

  /* Derive achieved sample rate from quantized ratio */
  uint32_t effective_ratio = ratio | ( ( ratio & 0x08000000 ) << 1 );
  dev->tuning.sampling_rate_hz = (uint32_t)( ( (uint64_t)xtal * ( 1ULL << 22 ) ) / effective_ratio );

  /* Emit ratio registers (demod page 1, 0x9F..0xA2) */
  int32_t rc = submit_demod_write( dev, 1, 0x9F, ( ratio >> 16 ) & 0xFFFF, 2 );
  if( rc != SDRGG_OK ) {
    return rc;
  }
  rc = submit_demod_write( dev, 1, 0xA1, ratio & 0xFFFF, 2 );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* DDC reset after rate change */
  rc = submit_demod_write( dev, 1, 0x01, 0x14, 1 );
  if( rc != SDRGG_OK ) {
    return rc;
  }
  rc = submit_demod_write( dev, 1, 0x01, 0x10, 1 );

  return rc;
}

/* ======================================================================
*  IF FREQUENCY CONFIGURATION
*  Programs the DDC NCO for desired IF offset.
* ====================================================================== */

int32_t rtl::set_if_freq( sdrgg_dev_t *dev, uint32_t if_freq_hz ) {
  uint32_t xtal = dev->baseband.oscillator_hz;
  int32_t nco_word;

  if( if_freq_hz == 0 ) {
    nco_word = 0;
  } else {
    nco_word = (int32_t)( ( (int64_t)if_freq_hz * ( 1LL << 22 ) ) / xtal );
    nco_word = -nco_word;
  }

  /* Write NCO word: demod page 1, registers 0x19..0x1B (3 bytes) */
  int32_t rc = submit_demod_write( dev, 1, 0x19, ( nco_word >> 16 ) & 0x3F, 1 );
  if( rc != SDRGG_OK ) {
    return rc;
  }
  rc = submit_demod_write( dev, 1, 0x1A, ( nco_word >> 8 ) & 0xFF, 1 );
  if( rc != SDRGG_OK ) {
    return rc;
  }
  rc = submit_demod_write( dev, 1, 0x1B, nco_word & 0xFF, 1 );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  dev->tuning.if_offset_hz = if_freq_hz;
  return SDRGG_OK;
}

/* ======================================================================
*  BULK STREAMING CONTROL
* ====================================================================== */

int32_t rtl::start_bulk( sdrgg_dev_t *dev ) {
  int32_t rc = submit_block_write( dev, SDRGG_BLOCK_USB, SDRGG_USB_EPA_CTL, 0x1002, 2 );
  if( rc != SDRGG_OK ) {
    return rc;
  }
  return submit_block_write( dev, SDRGG_BLOCK_USB, SDRGG_USB_EPA_CTL, 0x0000, 2 );
}

int32_t rtl::stop_bulk( sdrgg_dev_t *dev ) {
  return submit_block_write( dev, SDRGG_BLOCK_USB, SDRGG_USB_EPA_CTL, 0x1002, 2 );
}
