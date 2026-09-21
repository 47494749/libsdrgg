/*
 * sdrgg_ring.cpp - IQ ring buffer + multi-consumer streaming engine
 *
 * Architecture:
 *   Reader thread (1 per device) -> IQ ring buffer -> N consumer threads
 *
 * The reader thread acquires IQ data from the USB device and writes it
 * into a lock-free ring buffer. Consumer threads (subscribers) each
 * maintain their own read pointer and call user callbacks at their own
 * pace. If a consumer is too slow, it loses samples without affecting
 * other consumers.
 *
 * For R820T: reader uses async URBs (epoll) for maximum throughput.
 * For FC0012: reader uses sync bulk reads (1 URB at a time) to avoid
 * the hardware bug where async URBs corrupt the tuner after ~2 minutes.
 *
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "sdrgg_internal.h"

/* ======================================================================
 *  Ring buffer operations
 * ====================================================================== */

static int32_t ring_init( iq_ring *ring ) {
  ring->size = SDRGG_RING_SIZE;
  ring->data = (uint8_t *)malloc( ring->size );
  if( !ring->data ) return SDRGG_ERR_NOMEM;
  memset( ring->data, 128, ring->size );
  ring->write_pos.store( 0, std::memory_order_relaxed );
  pthread_mutex_init( &ring->sub_lock, NULL );
  for( int32_t i = 0; i < SDRGG_MAX_SUBSCRIBERS; i++ ) {
    ring->subs[i].active = false;
    ring->subs[i].thread_started = false;
    ring->subs[i].cancel.store( false );
    ring->subs[i].read_pos = 0;
    ring->subs[i].sequence = 0;
    ring->subs[i].dev = NULL;
  }
  return SDRGG_OK;
}

static void ring_destroy( iq_ring *ring ) {
  free( ring->data );
  ring->data = NULL;
  pthread_mutex_destroy( &ring->sub_lock );
}

static void ring_write( iq_ring *ring, const uint8_t *data, uint32_t len ) {
  uint32_t wp = ring->write_pos.load( std::memory_order_relaxed );
  uint32_t mask = ring->size - 1;

  uint32_t first = ring->size - (wp & mask);
  if( first >= len ) {
    memcpy( ring->data + (wp & mask), data, len );
  } else {
    memcpy( ring->data + (wp & mask), data, first );
    memcpy( ring->data, data + first, len - first );
  }

  ring->write_pos.store( wp + len, std::memory_order_release );
}

/* ======================================================================
 *  Consumer (subscriber) thread
 * ====================================================================== */

static void *consumer_thread_fn( void *arg ) {
  iq_subscriber *sub = (iq_subscriber *)arg;
  sdrgg_dev_t *dev = sub->dev;
  iq_ring *ring = &dev->stream.ring;

  uint32_t segment_size = 16384;
  uint8_t *delivery_buf = (uint8_t *)malloc( segment_size );
  if( !delivery_buf ) return NULL;

  while( !sub->cancel.load( std::memory_order_acquire ) ) {
    uint32_t wp = ring->write_pos.load( std::memory_order_acquire );
    uint32_t rp = sub->read_pos;
    uint32_t avail = wp - rp;

    if( avail < segment_size ) {
      if( dev->health.load() == DEV_HEALTH_DISCONNECTED )
        break;
      struct timespec ts = { 0, 1000000 };  /* 1ms */
      nanosleep( &ts, NULL );
      continue;
    }

    if( avail > ring->size ) {
      rp = wp - segment_size;
      sub->read_pos = rp;
      avail = segment_size;
    }

    uint32_t mask = ring->size - 1;
    uint32_t start = rp & mask;
    uint32_t first = ring->size - start;
    if( first >= segment_size ) {
      memcpy( delivery_buf, ring->data + start, segment_size );
    } else {
      memcpy( delivery_buf, ring->data + start, first );
      memcpy( delivery_buf + first, ring->data, segment_size - first );
    }

    sdrgg_buffer_t desc;
    desc.data = delivery_buf;
    desc.length = segment_size;
    desc.timestamp_us = 0;
    desc.sequence = sub->sequence++;
    sub->callback( dev, &desc, sub->user_ctx );

    sub->read_pos = rp + segment_size;
  }

  free( delivery_buf );
  return NULL;
}

/* ======================================================================
 *  Reader thread (1 per device)
 * ====================================================================== */

/* Callback for async URB completion — writes into ring buffer */
static void async_ring_cb( sdrgg_dev_t *dev, const sdrgg_buffer_t *buf, void * /* ctx */ ) {
  ring_write( &dev->stream.ring, buf->data, buf->length );
}

static void *reader_thread_fn( void *arg ) {
  sdrgg_dev_t *dev = (sdrgg_dev_t *)arg;
  bool use_sync = ( dev->identity.tuner_class == SDRGG_TUNER_FC0012 ||
                    dev->identity.tuner_class == SDRGG_TUNER_FC0013 );

  fprintf( stderr, "sdrgg: reader thread started for slot %d (%s)\n",
           dev->identity.slot_index,
           use_sync ? "sync" : "async" );

  if( use_sync ) {
    uint32_t buf_size = 16384;
    uint8_t *buf = (uint8_t *)malloc( buf_size );
    if( !buf ) return NULL;

    rtl::start_bulk( dev );

    while( !dev->stream.cancel_requested.load( std::memory_order_acquire ) ) {
      uint32_t actual = 0;
      int32_t rc = usb::bulk_read( dev, buf, buf_size, 1000, &actual );
      if( rc == SDRGG_OK && actual > 0 ) {
        ring_write( &dev->stream.ring, buf, actual );
      } else if( rc != SDRGG_OK ) {
        if( dev->health.load() == DEV_HEALTH_DISCONNECTED ) {
          fprintf( stderr, "sdrgg: reader thread slot %d: device disconnected, exiting\n",
                   dev->identity.slot_index );
          break;
        }
        usleep( 10000 );
      }
    }

    free( buf );
  } else {
    /* R820T: async URB streaming — set up URBs, event loop, then wait */
    uint32_t buf_count = SDRGG_STREAM_BUF_COUNT;
    uint32_t buf_size = SDRGG_STREAM_BUF_SIZE;

    int32_t rc = usb::urb_alloc( dev, buf_count, buf_size );
    if( rc != SDRGG_OK ) return NULL;

    dev->stream.callback = (sdrgg_stream_cb_t)async_ring_cb;
    dev->stream.user_data = NULL;

    rc = rtl::start_bulk( dev );
    if( rc != SDRGG_OK ) { usb::urb_free( dev ); return NULL; }

    rc = usb::urb_submit_all( dev );
    if( rc != SDRGG_OK ) { rtl::stop_bulk( dev ); usb::urb_free( dev ); return NULL; }

    sdrgg_ctx_t *ctx = dev->identity.ctx;

    pthread_mutex_lock( &ctx->lock );
    usb::event_loop_start( ctx );
    pthread_mutex_unlock( &ctx->lock );

    usb::event_loop_add_dev( ctx, dev );

    /* Wait until cancel is requested or device disconnects */
    while( !dev->stream.cancel_requested.load( std::memory_order_acquire ) ) {
      if( dev->health.load() == DEV_HEALTH_DISCONNECTED ) {
        fprintf( stderr, "sdrgg: reader thread slot %d: device disconnected (async), exiting\n",
                 dev->identity.slot_index );
        break;
      }
      usleep( 100000 );
    }

    /* Cleanup */
    usb::event_loop_remove_dev( ctx, dev );
    usb::urb_cancel_all( dev );

    if( dev->identity.tuner_class != SDRGG_TUNER_FC0012 &&
        dev->identity.tuner_class != SDRGG_TUNER_FC0013 ) {
      rtl::stop_bulk( dev );
    }
    usb::urb_free( dev );

    if( ctx->streaming_count.load() == 0 ) {
      usb::event_loop_stop( ctx );
    }
  }

  fprintf( stderr, "sdrgg: reader thread exiting for slot %d\n", dev->identity.slot_index );
  return NULL;
}

/* ======================================================================
 *  Public API: subscribe / unsubscribe
 * ====================================================================== */

namespace sdr {

int32_t subscribe( sdrgg_dev_t *dev, sdrgg_stream_cb_t callback, void *user_ctx ) {
  if( !dev || !callback ) return SDRGG_ERR_PARAM;
  if( !dev->stream.active.load() ) return SDRGG_ERR_PARAM;

  iq_ring *ring = &dev->stream.ring;
  pthread_mutex_lock( &ring->sub_lock );

  int32_t handle = -1;
  for( int32_t i = 0; i < SDRGG_MAX_SUBSCRIBERS; i++ ) {
    if( !ring->subs[i].active ) {
      handle = i;
      break;
    }
  }

  if( handle < 0 ) {
    pthread_mutex_unlock( &ring->sub_lock );
    return SDRGG_ERR_BUSY;
  }

  iq_subscriber *sub = &ring->subs[handle];
  sub->callback = callback;
  sub->user_ctx = user_ctx;
  sub->dev = dev;
  sub->cancel.store( false );
  sub->read_pos = ring->write_pos.load( std::memory_order_acquire );
  sub->sequence = 0;
  sub->active = true;

  if( pthread_create( &sub->thread, NULL, consumer_thread_fn, sub ) != 0 ) {
    sub->active = false;
    pthread_mutex_unlock( &ring->sub_lock );
    return SDRGG_ERR_IO;
  }
  sub->thread_started = true;

  pthread_mutex_unlock( &ring->sub_lock );
  return handle;
}

int32_t unsubscribe( sdrgg_dev_t *dev, int32_t handle ) {
  if( !dev || handle < 0 || handle >= SDRGG_MAX_SUBSCRIBERS ) return SDRGG_ERR_PARAM;

  iq_ring *ring = &dev->stream.ring;
  pthread_mutex_lock( &ring->sub_lock );

  iq_subscriber *sub = &ring->subs[handle];
  if( !sub->active ) {
    pthread_mutex_unlock( &ring->sub_lock );
    return SDRGG_ERR_PARAM;
  }

  sub->cancel.store( true, std::memory_order_release );
  pthread_mutex_unlock( &ring->sub_lock );

  if( sub->thread_started ) {
    pthread_join( sub->thread, NULL );
    sub->thread_started = false;
  }

  pthread_mutex_lock( &ring->sub_lock );
  sub->active = false;
  sub->dev = NULL;
  pthread_mutex_unlock( &ring->sub_lock );

  return SDRGG_OK;
}

} /* namespace sdr */

/* ======================================================================
 *  Ring engine: start / stop
 * ====================================================================== */

namespace ring_engine {

int32_t start( sdrgg_dev_t *dev, const sdrgg_stream_cfg_t *cfg,
               sdrgg_stream_cb_t callback, void *user_ctx ) {
  if( !dev || !callback ) return SDRGG_ERR_PARAM;
  if( dev->stream.active.load() ) return SDRGG_ERR_BUSY;

  (void)cfg;

  int32_t rc = ring_init( &dev->stream.ring );
  if( rc != SDRGG_OK ) return rc;

  dev->stream.cancel_requested.store( false );
  dev->stream.active.store( true );
  dev->stream.sequence = 0;

  if( pthread_create( &dev->stream.reader_thread, NULL, reader_thread_fn, dev ) != 0 ) {
    dev->stream.active.store( false );
    ring_destroy( &dev->stream.ring );
    return SDRGG_ERR_IO;
  }
  dev->stream.reader_started = true;

  /* Give reader thread a moment to start and set up the callback */
  usleep( 50000 );

  int32_t handle = sdr::subscribe( dev, callback, user_ctx );
  if( handle < 0 ) {
    dev->stream.cancel_requested.store( true );
    pthread_join( dev->stream.reader_thread, NULL );
    dev->stream.active.store( false );
    ring_destroy( &dev->stream.ring );
    return handle;
  }

  return SDRGG_OK;
}

int32_t stop( sdrgg_dev_t *dev ) {
  if( !dev || !dev->stream.active.load() ) return SDRGG_OK;

  for( int32_t i = 0; i < SDRGG_MAX_SUBSCRIBERS; i++ ) {
    if( dev->stream.ring.subs[i].active ) {
      sdr::unsubscribe( dev, i );
    }
  }

  dev->stream.cancel_requested.store( true, std::memory_order_release );
  if( dev->stream.reader_started ) {
    pthread_join( dev->stream.reader_thread, NULL );
    dev->stream.reader_started = false;
  }

  dev->stream.active.store( false );
  ring_destroy( &dev->stream.ring );

  return SDRGG_OK;
}

} /* namespace ring_engine */
