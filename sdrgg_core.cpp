/*
* sdrgg_core.cpp - Device session management and policy engine
*
* Architecture:
*   Probe graph       - weighted detection with confidence scores
*   Attach session    - phased device bring-up with rollback
*   Gain policy       - per-tuner-family gain distribution objects
*   Configuration API - frequency, sample rate, streaming lifecycle
*
* The core delegates all register operations to the baseband engine
* (rtl::) and tuner codecs (r820t::, fc0012::).
*
* License: MIT
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>

#include "sdrgg_internal.h"

/* ======================================================================
*  Tuner family capability contracts
*
*  Each tuner family exposes a contract of operations that the
*  core can invoke without knowing internal tuner details.
*  Contracts carry detection, activation, startup, and gain policy.
* ====================================================================== */

namespace {

struct tuner_family_contract {
  sdrgg_tuner_type_t family;
  uint16_t           bus_addr;
  uint8_t            priority;           /* lower = tried first */

  /* Lifecycle operations */
  int32_t ( *probe_fn )( sdrgg_dev_t *dev );
  int32_t ( *prepare_baseband_fn )( sdrgg_dev_t *dev );  /* may be NULL */
  int32_t ( *startup_fn )( sdrgg_dev_t *dev );

  /* Gain policy delegation */
  int32_t ( *apply_gain_fn )( sdrgg_dev_t *dev, int32_t gain_tenth_db );
  int32_t ( *apply_auto_gain_fn )( sdrgg_dev_t *dev );   /* may be NULL */
};

/* R820T gain policy: delegate to envelope-based selection */
static int32_t r820t_apply_gain( sdrgg_dev_t *dev, int32_t gain_tenth_db ) {
  const r820t::gain_profile *profile = r820t::select_gain_profile( gain_tenth_db );
  if( !profile ) {
    return SDRGG_ERR_PARAM;
  }

  int32_t rc = r820t::set_lna_gain( dev, profile->lna_index );
  if( rc == SDRGG_OK ) {
    rc = r820t::set_mixer_gain( dev, profile->mixer_index );
  }
  if( rc == SDRGG_OK ) {
    rc = r820t::set_vga_gain( dev, profile->vga_index );
  }
  return rc;
}

static int32_t r820t_apply_auto( sdrgg_dev_t *dev ) {
  int32_t rc = r820t::set_lna_gain( dev, -1 );
  if( rc == SDRGG_OK ) {
    rc = r820t::set_mixer_gain( dev, -1 );
  }
  return rc;
}

/* FC0012 gain policy: direct delegation */
static int32_t fc0012_apply_gain( sdrgg_dev_t *dev, int32_t gain_tenth_db ) {
  return fc0012::set_gain( dev, gain_tenth_db );
}

/* Capability contracts registry */
static const tuner_family_contract family_contracts[] = {
  {
    SDRGG_TUNER_R820T,
    SDRGG_R820T_I2C_ADDR,
    0,
    r820t::detect,
    rtl::configure_r820t,
    r820t::init,
    r820t_apply_gain,
    r820t_apply_auto,
  },
  {
    SDRGG_TUNER_FC0012,
    0xC6,
    1,
    fc0012::detect,
    nullptr,
    fc0012::init,
    fc0012_apply_gain,
    nullptr,
  },
};

static constexpr uint32_t NUM_FAMILY_CONTRACTS = sizeof( family_contracts ) / sizeof( family_contracts[0] );

/* Runtime: active contract for the attached device */
static const tuner_family_contract *active_contract( sdrgg_dev_t *dev ) {
  for( uint32_t i = 0; i < NUM_FAMILY_CONTRACTS; i++ ) {
    if( family_contracts[i].family == dev->identity.tuner_class ) {
      return &family_contracts[i];
    }
  }
  return nullptr;
}

} /* anonymous namespace */

/* ======================================================================
*  Session lifecycle
* ====================================================================== */

namespace sdr {

/* ---- Context management ---- */

sdrgg_ctx_t *create( void ) {
  sdrgg_ctx_t *ctx = (sdrgg_ctx_t *)calloc( 1, sizeof( sdrgg_ctx_t ) );
  if( !ctx ) {
    return NULL;
  }

  pthread_mutex_init( &ctx->lock, NULL );
  ctx->epoll_fd = -1;
  ctx->event_pipe[0] = -1;
  ctx->event_pipe[1] = -1;
  ctx->event_running = false;
  ctx->streaming_count = 0;
  return ctx;
}

void destroy( sdrgg_ctx_t *ctx ) {
  if( !ctx ) {
    return;
  }

  usb::event_loop_stop( ctx );

  /* Collect devices to close, then release without holding lock */
  sdrgg_dev_t *pending[SDRGG_MAX_DEVICES];
  int32_t n = 0;

  pthread_mutex_lock( &ctx->lock );
  for( int32_t i = 0; i < ctx->dev_count; i++ ) {
    if( ctx->devices[i] ) {
      pending[n++] = ctx->devices[i];
      ctx->devices[i] = NULL;
    }
  }
  pthread_mutex_unlock( &ctx->lock );

  for( int32_t i = 0; i < n; i++ ) {
    close( pending[i] );
  }

  pthread_mutex_destroy( &ctx->lock );
  free( ctx );
}

/* ---- Device attach (by index) ---- */

sdrgg_dev_t *open( sdrgg_ctx_t *ctx, int32_t index ) {
  sdrgg_devinfo_t devs[8];
  int32_t count = sdrgg_enumerate( ctx, devs, 8 );
  if( index < 0 || index >= count ) {
    return NULL;
  }

  return open_path( ctx, devs[index].path );
}

/* ---- Device attach session (by path) ----
*
*  The attach session is a state object that tracks phase completion
*  for automatic rollback. Phases:
*    Phase A: Identity establishment
*    Phase B: USB claim and baseband boot
*    Phase C: Family contract discovery (probe + activate + startup)
*    Phase D: Context registration
*
*  Any phase failure triggers rollback of all completed prior phases.
*/

struct attach_session {
  sdrgg_dev_t *dev;
  sdrgg_ctx_t *ctx;
  bool phase_a_done;   /* identity established, fd open */
  bool phase_b_done;   /* USB claimed, baseband initialized */
  bool phase_c_done;   /* tuner detected and started */
  bool phase_d_done;   /* registered in context */
};

static void rollback_session( attach_session *session ) {
  if( !session->dev ) return;

  if( session->phase_c_done ) {
    /* Tuner was started; attempt dormancy if R820T */
    if( session->dev->identity.tuner_class == SDRGG_TUNER_R820T ) {
      r820t::standby( session->dev );
    }
  }
  if( session->phase_b_done ) {
    rtl::deinit( session->dev );
    usb::release( session->dev );
  }
  if( session->phase_a_done ) {
    ::close( session->dev->identity.fd );
  }

  pthread_mutex_destroy( &session->dev->lock );
  free( session->dev );
  session->dev = NULL;
}

sdrgg_dev_t *open_path( sdrgg_ctx_t *ctx, const char *path ) {
  if( !ctx || !path ) {
    return NULL;
  }

  attach_session session = {};
  session.ctx = ctx;

  /* Phase A: Identity establishment */
  int32_t fd = ::open( path, O_RDWR );
  if( fd < 0 ) {
    return NULL;
  }

  sdrgg_dev_t *dev = (sdrgg_dev_t *)calloc( 1, sizeof( sdrgg_dev_t ) );
  if( !dev ) { ::close( fd ); return NULL; }

  session.dev = dev;
  dev->identity.ctx = ctx;
  dev->identity.fd = fd;
  dev->baseband.oscillator_hz = SDRGG_XTAL_FREQ;
  dev->identity.bulk_endpoint = SDRGG_USB_EPA;
  dev->baseband.freq_offset_ppm = 0;
  pthread_mutex_init( &dev->lock, NULL );

  strncpy( dev->identity.info.path, path, sizeof( dev->identity.info.path ) - 1 );

  /* Extract bus topology from device path */
  {
    int32_t bus = 0, addr = 0;
    if( sscanf( path, "/dev/bus/usb/%d/%d", &bus, &addr ) == 2 ) {
      dev->identity.info.bus = (uint8_t)bus;
      dev->identity.info.devaddr = (uint8_t)addr;
    }
  }

  /* Read USB descriptor for identification */
  {
    uint8_t desc[18];
    struct usbdevfs_ctrltransfer ctrl = {
      .bRequestType = 0x80,
      .bRequest = 6,
      .wValue = 0x0100,
      .wIndex = 0,
      .wLength = 18,
      .timeout = SDRGG_USB_TIMEOUT_MS,
      .data = desc,
    };
    if( ioctl( fd, USBDEVFS_CONTROL, &ctrl ) >= 18 ) {
      dev->identity.info.vid = (uint16_t)( desc[8] | ( desc[9] << 8 ) );
      dev->identity.info.pid = (uint16_t)( desc[10] | ( desc[11] << 8 ) );
    }
  }

  session.phase_a_done = true;

  /* Phase B: USB claim and baseband boot */
  if( usb::claim( dev ) != SDRGG_OK ) {
    rollback_session( &session );
    return NULL;
  }

  if( rtl::init( dev ) != SDRGG_OK ) {
    usb::release( dev );
    rollback_session( &session );
    return NULL;
  }

  session.phase_b_done = true;

  /* Phase C: Family contract discovery */
  bool detected = false;

  for( uint32_t p = 0; p < NUM_FAMILY_CONTRACTS; p++ ) {
    const tuner_family_contract *contract = &family_contracts[p];
    dev->identity.tuner_bus_addr = contract->bus_addr;

    if( contract->probe_fn( dev ) == SDRGG_OK ) {
      dev->identity.tuner_class = contract->family;
      dev->identity.info.tuner = contract->family;

      /* Apply baseband preparation if contract requires it */
      if( contract->prepare_baseband_fn ) {
        if( contract->prepare_baseband_fn( dev ) != SDRGG_OK ) {
          break;
        }
      }

      /* Run tuner startup */
      if( contract->startup_fn( dev ) != SDRGG_OK ) {
        break;
      }

      detected = true;
      break;
    }
  }

  if( !detected ) {
    rollback_session( &session );
    return NULL;
  }

  session.phase_c_done = true;

  /* Phase D: Context registration */
  pthread_mutex_lock( &ctx->lock );
  int32_t slot = -1;
  for( int32_t i = 0; i < SDRGG_MAX_DEVICES; i++ ) {
    if( ctx->devices[i] == NULL ) {
      slot = i;
      break;
    }
  }
  if( slot < 0 ) {
    pthread_mutex_unlock( &ctx->lock );
    rollback_session( &session );
    return NULL;
  }
  dev->identity.slot_index = slot;
  ctx->devices[slot] = dev;
  if( slot >= ctx->dev_count ) {
    ctx->dev_count = slot + 1;
  }
  pthread_mutex_unlock( &ctx->lock );

  session.phase_d_done = true;
  return dev;
}

/* ---- Device detach ---- */

void close( sdrgg_dev_t *dev ) {
  if( !dev ) {
    return;
  }

  /* Unregister from context */
  if( dev->identity.ctx ) {
    pthread_mutex_lock( &dev->identity.ctx->lock );
    for( int32_t i = 0; i < dev->identity.ctx->dev_count; i++ ) {
      if( dev->identity.ctx->devices[i] == dev ) {
        dev->identity.ctx->devices[i] = NULL;
        break;
      }
    }
    pthread_mutex_unlock( &dev->identity.ctx->lock );
  }

  /* Halt streaming if active */
  if( dev->stream.active ) {
    stop_stream( dev );
  }

  /* Tuner dormancy transition */
  if( dev->identity.tuner_class == SDRGG_TUNER_R820T || dev->identity.tuner_class == SDRGG_TUNER_R820T2 ) {
    r820t::standby( dev );
  }

  /* Baseband shutdown */
  rtl::deinit( dev );

  /* Release USB resources */
  usb::release( dev );
  ::close( dev->identity.fd );
  usb::urb_free( dev );

  pthread_mutex_destroy( &dev->lock );
  free( dev );
}

/* ======================================================================
*  Configuration API
* ====================================================================== */

int32_t set_frequency( sdrgg_dev_t *dev, uint32_t freq_hz, uint32_t *actual_hz ) {
  if( !dev ) {
    return SDRGG_ERR_PARAM;
  }

  pthread_mutex_lock( &dev->lock );

  /* Record requested state before hardware attempt */
  dev->tuning.requested_freq_hz = freq_hz;

  int32_t rc;
  if( dev->identity.tuner_class == SDRGG_TUNER_FC0012 ) {
    rc = fc0012::set_freq( dev, freq_hz );
  } else {
    rc = r820t::set_freq( dev, freq_hz, dev->tuning.if_offset_hz );
  }

  /* Track commit outcome */
  dev->tuning.last_freq_result = rc;

  if( rc == SDRGG_OK ) {
    dev->tuning.center_freq_hz = freq_hz;
    if( actual_hz ) {
      *actual_hz = freq_hz;
    }
  }

  pthread_mutex_unlock( &dev->lock );
  return rc;
}

int32_t set_sample_rate( sdrgg_dev_t *dev, uint32_t rate_hz, uint32_t *actual_hz ) {
  if( !dev ) {
    return SDRGG_ERR_PARAM;
  }

  pthread_mutex_lock( &dev->lock );

  /* Record requested state before hardware attempt */
  dev->tuning.requested_rate_hz = rate_hz;

  int32_t rc = rtl::set_sample_rate( dev, rate_hz );

  /* Track commit outcome */
  dev->tuning.last_rate_result = rc;

  if( rc == SDRGG_OK && actual_hz ) {
    *actual_hz = dev->tuning.sampling_rate_hz;
  }
  pthread_mutex_unlock( &dev->lock );

  return rc;
}

/* ---- Gain policy application (delegated to family contract) ---- */

int32_t set_gain( sdrgg_dev_t *dev, int32_t gain_tenth_db ) {
  if( !dev ) {
    return SDRGG_ERR_PARAM;
  }

  pthread_mutex_lock( &dev->lock );
  int32_t rc;

  const tuner_family_contract *contract = active_contract( dev );
  if( !contract ) {
    pthread_mutex_unlock( &dev->lock );
    return SDRGG_ERR_PARAM;
  }

  if( gain_tenth_db == SDRGG_GAIN_AUTO && contract->apply_auto_gain_fn ) {
    /* Auto policy: delegate to family's AGC activation */
    rc = contract->apply_auto_gain_fn( dev );
    dev->tuning.gain_policy = -1;
  } else {
    /* Manual policy: delegate to family's gain selection */
    rc = contract->apply_gain_fn( dev, gain_tenth_db );
    dev->tuning.gain_policy = gain_tenth_db;
  }

  pthread_mutex_unlock( &dev->lock );
  return rc;
}

int32_t get_gain( sdrgg_dev_t *dev, int32_t *gain_tenth_db ) {
  if( !dev || !gain_tenth_db ) {
    return SDRGG_ERR_PARAM;
  }
  *gain_tenth_db = dev->tuning.gain_policy;
  return SDRGG_OK;
}

int32_t set_freq_correction( sdrgg_dev_t *dev, int32_t ppm ) {
  if( !dev ) {
    return SDRGG_ERR_PARAM;
  }
  dev->baseband.freq_offset_ppm = ppm;
  return SDRGG_OK;
}

int32_t set_digital_agc( sdrgg_dev_t *dev, bool enable ) {
  if( !dev ) {
    return SDRGG_ERR_PARAM;
  }
  uint8_t val = enable ? 0x25 : 0x05;
  return demod::write( dev, SDRGG_BLOCK_DEMOD, 0x0019, val );
}

/* ======================================================================
*  Streaming (async URB + epoll event loop)
* ====================================================================== */

int32_t start_stream( sdrgg_dev_t *dev, const sdrgg_stream_cfg_t *cfg, sdrgg_stream_cb_t callback, void *user_ctx ) {
  if( !dev || !callback ) {
    return SDRGG_ERR_PARAM;
  }
  if( dev->stream.active ) {
    return SDRGG_ERR_BUSY;
  }

  sdrgg_ctx_t *ctx = dev->identity.ctx;
  if( !ctx ) {
    return SDRGG_ERR_PARAM;
  }

  /* Apply configuration with defaults */
  uint32_t buf_count = SDRGG_STREAM_BUF_COUNT;
  uint32_t buf_size = SDRGG_STREAM_BUF_SIZE;

  if( cfg ) {
    if( cfg->buf_count > 0 ) {
      buf_count = cfg->buf_count;
    }
    if( cfg->buf_size >= SDRGG_MIN_BUF_SIZE && cfg->buf_size <= SDRGG_MAX_BUF_SIZE ) {
      buf_size = cfg->buf_size;
    }
  }

  /* Allocate DMA buffer pool */
  int32_t rc = usb::urb_alloc( dev, buf_count, buf_size );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  dev->stream.callback = callback;
  dev->stream.user_data = user_ctx;
  dev->stream.cancel_requested = false;
  dev->stream.sequence = 0;
  dev->stream.active = true;

  /* Ensure event loop is running */
  pthread_mutex_lock( &ctx->lock );
  rc = usb::event_loop_start( ctx );
  pthread_mutex_unlock( &ctx->lock );
  if( rc != SDRGG_OK ) {
    usb::urb_free( dev );
    dev->stream.active = false;
    return rc;
  }

  /* Reset endpoint and begin data flow */
  rc = rtl::start_bulk( dev );
  if( rc != SDRGG_OK ) {
    usb::urb_free( dev );
    dev->stream.active = false;
    return rc;
  }

  /* Submit all URBs for DMA */
  rc = usb::urb_submit_all( dev );
  if( rc != SDRGG_OK ) {
    rtl::stop_bulk( dev );
    usb::urb_free( dev );
    dev->stream.active = false;
    return rc;
  }

  /* Register with epoll event loop */
  rc = usb::event_loop_add_dev( ctx, dev );
  if( rc != SDRGG_OK ) {
    usb::urb_cancel_all( dev );
    rtl::stop_bulk( dev );
    usb::urb_free( dev );
    dev->stream.active = false;
    return rc;
  }

  return SDRGG_OK;
}

int32_t stop_stream( sdrgg_dev_t *dev ) {
  if( !dev || !dev->stream.active ) {
    return SDRGG_OK;
  }

  sdrgg_ctx_t *ctx = dev->identity.ctx;

  /* Signal cancellation */
  dev->stream.cancel_requested = true;
  dev->stream.active = false;

  /* Remove from event loop */
  if( ctx ) {
    usb::event_loop_remove_dev( ctx, dev );
  }

  /* Cancel in-flight URBs */
  usb::urb_cancel_all( dev );

  /* Stop endpoint */
  rtl::stop_bulk( dev );

  /* Release buffer pool */
  usb::urb_free( dev );

  /* Stop event loop if no more active streams */
  if( ctx ) {
    if( ctx->streaming_count.load() == 0 ) {
      usb::event_loop_stop( ctx );
    }
  }

  return SDRGG_OK;
}

/* ---- Synchronous read ---- */

int32_t read_sync( sdrgg_dev_t *dev, uint8_t *buf, uint32_t max_bytes, uint32_t timeout_ms ) {
  if( !dev || !buf ) {
    return SDRGG_ERR_PARAM;
  }

  if( dev->stream.active ) {
    return SDRGG_ERR_BUSY;
  }

  int32_t rc = rtl::start_bulk( dev );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  uint32_t actual = 0;
  rc = usb::bulk_read( dev, buf, max_bytes, timeout_ms, &actual );

  rtl::stop_bulk( dev );

  if( rc != SDRGG_OK ) {
    return rc;
  }
  return (int32_t)actual;
}

/* ======================================================================
*  Info queries
* ====================================================================== */

sdrgg_tuner_type_t get_tuner_type( sdrgg_dev_t *dev ) {
  return dev ? dev->identity.tuner_class : SDRGG_TUNER_UNKNOWN;
}

int32_t get_devinfo( sdrgg_dev_t *dev, sdrgg_devinfo_t *info ) {
  if( !dev || !info ) {
    return SDRGG_ERR_PARAM;
  }
  *info = dev->identity.info;
  return SDRGG_OK;
}

uint32_t get_xtal_freq( sdrgg_dev_t *dev ) {
  return dev ? dev->baseband.oscillator_hz : 0;
}

int32_t get_tuner_caps( sdrgg_dev_t *dev, const tuner_caps **caps ) {
  if( !dev || !caps ) {
    return SDRGG_ERR_PARAM;
  }

  const tuner_caps *c = get_caps_by_type( dev->identity.tuner_class );
  if( !c ) {
    *caps = nullptr;
    return SDRGG_ERR_NODEV;
  }
  *caps = c;
  return SDRGG_OK;
}

const tuner_caps *get_caps_by_type( sdrgg_tuner_type_t type ) {
  switch( type ) {
  case SDRGG_TUNER_R820T:
  case SDRGG_TUNER_R820T2:
    return r820t::get_caps();
  case SDRGG_TUNER_FC0012:
    return fc0012::get_caps();
  case SDRGG_TUNER_FC0013:
    return fc0013::get_caps();
  case SDRGG_TUNER_FC2580:
    return fc2580::get_caps();
  case SDRGG_TUNER_E4000:
    return e4000::get_caps();
  default:
    return nullptr;
  }
}

} /* namespace sdr */
