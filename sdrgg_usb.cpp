/*
* sdrgg_usb.cpp - Hardware bus layer (hwbus)
*
* Direct kernel USB access via usbdevfs ioctl (no libusb).
* Organized into three sub-layers:
*   1. Control path  - synchronous vendor control transfers
*   2. Streaming path - async bulk URBs with zero-copy DMA
*   3. Enumeration   - sysfs-based device discovery
*
* The epoll event loop multiplexes all streaming devices on a single
* thread with sub-millisecond latency.
*
* License: MIT
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <time.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>

#include "sdrgg_internal.h"

/* ======================================================================
*  SUB-LAYER 1: Control path (synchronous USB transfers)
* ====================================================================== */

int32_t usb::control_write( sdrgg_dev_t *dev, uint16_t value, uint16_t index, const uint8_t *data, uint16_t len ) {
  struct usbdevfs_ctrltransfer ctrl = {
    .bRequestType = SDRGG_CTRL_OUT,
    .bRequest = 0,
    .wValue = value,
    .wIndex = index,
    .wLength = len,
    .timeout = SDRGG_USB_TIMEOUT_MS,
    .data = (void *)data,
  };

  int32_t rc = ioctl( dev->identity.fd, USBDEVFS_CONTROL, &ctrl );
  if( rc < 0 ) {
    return SDRGG_ERR_USB;
  }
  return SDRGG_OK;
}

int32_t usb::control_read( sdrgg_dev_t *dev, uint16_t value, uint16_t index, uint8_t *data, uint16_t len ) {
  struct usbdevfs_ctrltransfer ctrl = {
    .bRequestType = SDRGG_CTRL_IN,
    .bRequest = 0,
    .wValue = value,
    .wIndex = index,
    .wLength = len,
    .timeout = SDRGG_USB_TIMEOUT_MS,
    .data = data,
  };

  int32_t rc = ioctl( dev->identity.fd, USBDEVFS_CONTROL, &ctrl );
  if( rc < 0 ) {
    return SDRGG_ERR_USB;
  }
  return SDRGG_OK;
}

/* ---- Synchronous bulk (used only by read_sync path) ---- */

int32_t usb::bulk_read( sdrgg_dev_t *dev, uint8_t *buf, uint32_t len, uint32_t timeout_ms, uint32_t *actual ) {
  /* USBDEVFS_BULK ioctl limits per-transfer size to 16384 bytes.
   * Fragment larger reads into a loop of max-size chunks. */
  static const uint32_t MAX_CHUNK = 16384;
  uint32_t total = 0;

  while( total < len ) {
    uint32_t chunk = len - total;
    if( chunk > MAX_CHUNK ) chunk = MAX_CHUNK;

    struct usbdevfs_bulktransfer bulk = {
      .ep = SDRGG_USB_EPA,
      .len = chunk,
      .timeout = timeout_ms,
      .data = buf + total,
    };

    int32_t rc = ioctl( dev->identity.fd, USBDEVFS_BULK, &bulk );
    if( rc < 0 ) {
      /* If we already got some data, return what we have */
      if( total > 0 ) break;
      if( actual ) *actual = 0;
      return SDRGG_ERR_IO;
    }
    total += (uint32_t)rc;
    /* Short read: device has no more data ready */
    if( (uint32_t)rc < chunk ) break;
  }

  if( actual ) *actual = total;
  return SDRGG_OK;
}

/* ---- Interface claim/release ---- */

int32_t usb::claim( sdrgg_dev_t *dev ) {
  int32_t interface = 0;

  /* Detach kernel driver if attached */
  struct usbdevfs_getdriver getdrv = {};
  getdrv.interface = interface;
  if( ioctl( dev->identity.fd, USBDEVFS_GETDRIVER, &getdrv ) == 0 ) {
    struct usbdevfs_ioctl disc = {
      .ifno = interface,
      .ioctl_code = USBDEVFS_DISCONNECT,
      .data = NULL,
    };
    ioctl( dev->identity.fd, USBDEVFS_IOCTL, &disc );
  }

  if( ioctl( dev->identity.fd, USBDEVFS_CLAIMINTERFACE, &interface ) < 0 ) {
    return SDRGG_ERR_BUSY;
  }

  return SDRGG_OK;
}

int32_t usb::release( sdrgg_dev_t *dev ) {
  int32_t interface = 0;
  ioctl( dev->identity.fd, USBDEVFS_RELEASEINTERFACE, &interface );
  return SDRGG_OK;
}

/* ======================================================================
*  SUB-LAYER 2: Streaming path (async URBs + epoll event loop)
*
*  USBDEVFS_SUBMITURB pushes a URB to the kernel for DMA.
*  Completion makes the fd pollable (EPOLLOUT on usbdevfs).
*  USBDEVFS_REAPURBNDELAY harvests completed URBs non-blocking.
* ====================================================================== */

int32_t usb::urb_alloc( sdrgg_dev_t *dev, uint32_t count, uint32_t buf_size ) {
  dev->pipeline.arena = (sdrgg_urb_t *)calloc( count, sizeof( sdrgg_urb_t ) );
  if( !dev->pipeline.arena ) {
    return SDRGG_ERR_NOMEM;
  }

  dev->pipeline.arena_depth = count;
  dev->pipeline.segment_size = buf_size;
  dev->pipeline.submitted = 0;
  dev->pipeline.completed = 0;
  dev->pipeline.dropped = 0;
  dev->pipeline.backpressure = false;

  for( uint32_t i = 0; i < count; i++ ) {
    sdrgg_urb_t *u = &dev->pipeline.arena[i];
    u->dev = dev;
    u->index = i;
    u->buf_size = buf_size;
    u->submitted = false;
    u->urb = (struct usbdevfs_urb *)calloc( 1, sizeof( *u->urb ) );
    if( !u->urb ) {
      for( uint32_t j = 0; j < i; j++ ) {
        free( dev->pipeline.arena[j].buffer );
        free( dev->pipeline.arena[j].urb );
      }
      free( dev->pipeline.arena );
      dev->pipeline.arena = NULL;
      return SDRGG_ERR_NOMEM;
    }

    /* Page-aligned allocation for DMA.
    * The kernel can DMA directly into this buffer without copying. */
    if( posix_memalign( ( void ** )&u->buffer, 4096, buf_size ) != 0 ) {
      /* Cleanup already allocated */
      for( uint32_t j = 0; j < i; j++ ) {
        free( dev->pipeline.arena[j].buffer );
        free( dev->pipeline.arena[j].urb );
      }
      free( u->urb );
      free( dev->pipeline.arena );
      dev->pipeline.arena = NULL;
      return SDRGG_ERR_NOMEM;
    }

    /* Pre-fill URB structure */
    memset( u->urb, 0, sizeof( *u->urb ) );
    u->urb->type = USBDEVFS_URB_TYPE_BULK;
    u->urb->endpoint = SDRGG_USB_EPA;
    u->urb->buffer = u->buffer;
    u->urb->buffer_length = buf_size;
    /* usercontext points back to our tracking struct */
    u->urb->usercontext = u;
  }

  return SDRGG_OK;
}

void usb::urb_free( sdrgg_dev_t *dev ) {
  if( !dev->pipeline.arena ) {
    return;
  }

  /* Cancel any in-flight URBs first */
  usb::urb_cancel_all( dev );

  for( uint32_t i = 0; i < dev->pipeline.arena_depth; i++ ) {
    free( dev->pipeline.arena[i].buffer );
    free( dev->pipeline.arena[i].urb );
  }
  free( dev->pipeline.arena );
  dev->pipeline.arena = NULL;
  dev->pipeline.arena_depth = 0;
  dev->pipeline.submitted = 0;
}

int32_t usb::urb_submit( sdrgg_dev_t *dev, sdrgg_urb_t *u ) {
  if( u->submitted ) {
    return SDRGG_ERR_BUSY;
  }

  /* Reset URB fields for resubmission */
  u->urb->actual_length = 0;
  u->urb->status = 0;

  int32_t rc = ioctl( dev->identity.fd, USBDEVFS_SUBMITURB, u->urb );
  if( rc < 0 ) {
    return SDRGG_ERR_IO;
  }

  u->submitted = true;
  dev->pipeline.submitted++;
  dev->pipeline.backpressure = ( dev->pipeline.submitted >= dev->pipeline.arena_depth );
  return SDRGG_OK;
}

int32_t usb::urb_submit_all( sdrgg_dev_t *dev ) {
  for( uint32_t i = 0; i < dev->pipeline.arena_depth; i++ ) {
    int32_t rc = usb::urb_submit( dev, &dev->pipeline.arena[i] );
    if( rc != SDRGG_OK ) {
      return rc;
    }
  }
  return SDRGG_OK;
}

int32_t usb::urb_cancel_all( sdrgg_dev_t *dev ) {
  if( !dev->pipeline.arena ) {
    return SDRGG_OK;
  }

  /* Discard all submitted URBs */
  for( uint32_t i = 0; i < dev->pipeline.arena_depth; i++ ) {
    if( dev->pipeline.arena[i].submitted ) {
      ioctl( dev->identity.fd, USBDEVFS_DISCARDURB, dev->pipeline.arena[i].urb );
    }
  }

  /* Reap all discarded URBs to clean up */
  for( uint32_t i = 0; i < dev->pipeline.arena_depth; i++ ) {
    if( dev->pipeline.arena[i].submitted ) {
      struct usbdevfs_urb *reaped = NULL;
      /* Use blocking REAPURB since we know they should return quickly
      * after DISCARDURB */
      if( ioctl( dev->identity.fd, USBDEVFS_REAPURB, &reaped ) == 0 && reaped ) {
        sdrgg_urb_t *u = (sdrgg_urb_t *)reaped->usercontext;
        if( u ) {
          u->submitted = false;
          if( dev->pipeline.submitted > 0 ) {
            dev->pipeline.submitted--;
          }
        }
      }
    }
  }

  /* Force-clear state in case some reaps failed */
  for( uint32_t i = 0; i < dev->pipeline.arena_depth; i++ ) {
    dev->pipeline.arena[i].submitted = false;
  }
  dev->pipeline.submitted = 0;

  return SDRGG_OK;
}

sdrgg_urb_t *usb::urb_reap( sdrgg_dev_t *dev ) {
  struct usbdevfs_urb *reaped = NULL;

  int32_t rc = ioctl( dev->identity.fd, USBDEVFS_REAPURBNDELAY, &reaped );
  if( rc < 0 || !reaped ) {
    return NULL;
  }

  sdrgg_urb_t *u = (sdrgg_urb_t *)reaped->usercontext;
  if( u ) {
    u->submitted = false;
    if( dev->pipeline.submitted > 0 ) {
      dev->pipeline.submitted--;
    }
    dev->pipeline.backpressure = false;

    /* Accounting: classify reap as completion or drop */
    if( u->urb->status == 0 && u->urb->actual_length > 0 ) {
      dev->pipeline.completed++;
    } else {
      dev->pipeline.dropped++;
    }
  }
  return u;
}

/* ---- Event loop (epoll multi-device multiplexer) ----
*
* Single thread services all streaming devices. Each device fd
* added to one epoll instance. Pipe for wakeup signaling.
* Benefits: one thread for N dongles, deterministic latency,
* coherent timestamps across devices.
*/

static uint64_t monotonic_us( void ) {
  struct timespec ts;
  clock_gettime( CLOCK_MONOTONIC, &ts );
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void process_device_urbs( sdrgg_dev_t *dev ) {
  sdrgg_urb_t *u;

  /* Reap all completed URBs for this device */
  while( ( u = usb::urb_reap( dev ) ) != NULL ) {
    if( dev->stream.cancel_requested ) {
      continue; /* draining after cancel, don't callback */
    }

    if( u->urb->status == 0 && u->urb->actual_length > 0 ) {
      /* Deliver buffer to user — data pointer is the DMA buffer itself,
      * no memcpy needed (zero-copy) */
      sdrgg_buffer_t desc = {
        .data = u->buffer,
        .length = (uint32_t)u->urb->actual_length,
        .timestamp_us = monotonic_us(),
        .sequence = dev->stream.sequence++,
      };

      if( dev->stream.callback ) {
        dev->stream.callback( dev, &desc, dev->stream.user_data );
      }
    } else {
      /* URB completed with error or zero length */
#if SDRGG_ENABLE_DIAGNOSTICS
      if( dev->pipeline.dropped <= 3 ) {
        fprintf( stderr, "sdrgg-urb-diag: slot=%d status=%d actual=%d dropped=%u\n",
         dev->identity.slot_index, u->urb->status,
         u->urb->actual_length, dev->pipeline.dropped );
      }
#endif
    }

    /* Resubmit URB immediately (keeps pipeline full) */
    if( !dev->stream.cancel_requested ) {
      if( usb::urb_submit( dev, u ) != SDRGG_OK ) {
        /* URB lost â€” if all URBs drain, stream will stall.
        * Mark cancel so the user callback stops. */
        if( dev->pipeline.submitted == 0 ) {
          dev->stream.cancel_requested = true;
        }
      }
    }
  }
}

static void *event_loop_thread( void *arg ) {
  sdrgg_ctx_t *ctx = (sdrgg_ctx_t *)arg;
  struct epoll_event events[SDRGG_MAX_DEVICES + 1];

  while( ctx->event_running ) {
    /* 1ms timeout for low latency; usbdevfs signals EPOLLOUT on URB completion */
    int32_t nfds = epoll_wait( ctx->epoll_fd, events, SDRGG_MAX_DEVICES + 1, 1 /* ms timeout */ );

    if( nfds < 0 ) {
      if( errno == EINTR ) {
        continue;
      }
      break; /* fatal error */
    }

    for( int32_t i = 0; i < nfds; i++ ) {
      /* Check if it's the wakeup pipe */
      if( events[i].data.ptr == ctx ) {
        char dummy;
        ssize_t pipe_rc = read( ctx->event_pipe[0], &dummy, 1 );
        ( void )pipe_rc;
        continue;
      }

      /* It's a device fd with completed URBs */
      sdrgg_dev_t *dev = (sdrgg_dev_t *)events[i].data.ptr;
      if( dev && dev->stream.active ) {
        process_device_urbs( dev );
      }
    }

    /* Polling fallback: on every iteration (including timeout), try
    * reaping URBs from all streaming devices. Some kernel versions
    * (5.4) don't reliably signal EPOLLOUT on usbdevfs async URBs.
    * REAPURBNDELAY is non-blocking so this is safe and fast.
    * Hold ctx->lock to prevent sdrgg_close from freeing a device
    * while we're still accessing it. */
    pthread_mutex_lock( &ctx->lock );
    for( int32_t i = 0; i < SDRGG_MAX_DEVICES; i++ ) {
      sdrgg_dev_t *dev = ctx->devices[i];
      if( dev && dev->stream.active && !dev->stream.cancel_requested ) {
        process_device_urbs( dev );
      }
    }
    pthread_mutex_unlock( &ctx->lock );
  }

  return NULL;
}

int32_t usb::event_loop_start( sdrgg_ctx_t *ctx ) {
  if( ctx->event_running.load() ) {
    return SDRGG_OK;
  }

  ctx->epoll_fd = epoll_create1( EPOLL_CLOEXEC );
  if( ctx->epoll_fd < 0 ) {
    return SDRGG_ERR_IO;
  }

  if( pipe2( ctx->event_pipe, O_NONBLOCK | O_CLOEXEC ) < 0 ) {
    close( ctx->epoll_fd );
    ctx->epoll_fd = -1;
    return SDRGG_ERR_IO;
  }

  /* Add the wakeup pipe read-end to epoll */
  struct epoll_event ev = {};
  ev.events = EPOLLIN;
  ev.data.ptr = ctx; /* identifies pipe events vs device events */
  epoll_ctl( ctx->epoll_fd, EPOLL_CTL_ADD, ctx->event_pipe[0], &ev );

  ctx->event_running = true;

  if( pthread_create( &ctx->event_thread, NULL, event_loop_thread, ctx ) != 0 ) {
    ctx->event_running = false;
    close( ctx->event_pipe[0] );
    close( ctx->event_pipe[1] );
    close( ctx->epoll_fd );
    ctx->epoll_fd = -1;
    return SDRGG_ERR_IO;
  }

  return SDRGG_OK;
}

void usb::event_loop_stop( sdrgg_ctx_t *ctx ) {
  /* Atomically swap event_running to false. If it was already false,
  * another thread beat us (or the loop was never started) â€” bail out.
  * This prevents double-join (pthread_join on same thread twice is UB). */
  if( !atomic_exchange( &ctx->event_running, false ) ) {
    return;
  }

  /* Wake up the event loop so it exits */
  char c = 'q';
  ssize_t wake_rc = write( ctx->event_pipe[1], &c, 1 );
  ( void )wake_rc;

  pthread_join( ctx->event_thread, NULL );

  close( ctx->event_pipe[0] );
  close( ctx->event_pipe[1] );
  close( ctx->epoll_fd );
  ctx->epoll_fd = -1;
  ctx->event_pipe[0] = -1;
  ctx->event_pipe[1] = -1;
}

int32_t usb::event_loop_add_dev( sdrgg_ctx_t *ctx, sdrgg_dev_t *dev ) {
  /* usbdevfs signals EPOLLOUT (not EPOLLIN) when async URBs complete */
  struct epoll_event ev = {};
  ev.events = EPOLLOUT;
  ev.data.ptr = dev;

  int32_t rc = epoll_ctl( ctx->epoll_fd, EPOLL_CTL_ADD, dev->identity.fd, &ev );
  if( rc < 0 ) {
    return SDRGG_ERR_IO;
  }

  ctx->streaming_count++;

  /* Wake up event loop to pick up the new fd */
  char c = 'a';
  ssize_t wake_rc = write( ctx->event_pipe[1], &c, 1 );
  ( void )wake_rc;

  return SDRGG_OK;
}

int32_t usb::event_loop_remove_dev( sdrgg_ctx_t *ctx, sdrgg_dev_t *dev ) {
  epoll_ctl( ctx->epoll_fd, EPOLL_CTL_DEL, dev->identity.fd, NULL );

  if( ctx->streaming_count > 0 ) {
    ctx->streaming_count--;
  }

  return SDRGG_OK;
}

/* ======================================================================
*  SUB-LAYER 3: Enumeration (sysfs-based device discovery)
* ====================================================================== */

static int32_t read_sysfs_attr_u16( const char *dir, const char *attr, uint16_t *out ) {
  char path[256];
  snprintf( path, sizeof( path ), "%s/%s", dir, attr );

  int32_t fd = open( path, O_RDONLY );
  if( fd < 0 ) {
    return -1;
  }

  char buf[16];
  int32_t n = read( fd, buf, sizeof( buf ) - 1 );
  close( fd );
  if( n <= 0 ) {
    return -1;
  }

  buf[n] = '\0';
  *out = (uint16_t)strtoul(buf, NULL, 16);
  return 0;
}

static int32_t read_sysfs_attr_str( const char *dir, const char *attr, char *out, size_t out_sz ) {
  char path[256];
  snprintf( path, sizeof( path ), "%s/%s", dir, attr );

  int32_t fd = open( path, O_RDONLY );
  if( fd < 0 ) { out[0] = '\0'; return -1; }

  int32_t n = read( fd, out, out_sz - 1 );
  close( fd );
  if( n <= 0 ) { out[0] = '\0'; return -1; }

  out[n] = '\0';
  /* strip trailing newline */
  while( n > 0 && ( out[n-1] == '\n' || out[n-1] == '\r' ) ) {
    out[--n] = '\0';
  }
  return 0;
}

int32_t usb::enumerate( sdrgg_ctx_t *ctx, sdrgg_devinfo_t *devs, int32_t max_devs ) {
  ( void )ctx;
  int32_t found = 0;
  DIR *d = opendir( "/sys/bus/usb/devices" );
  if( !d ) {
    return 0;
  }

  struct dirent *ent;
  while( ( ent = readdir( d ) ) != NULL && found < max_devs ) {
    if( ent->d_name[0] == '.' ) {
      continue;
    }

    char sysdir[256];
    int32_t sysdir_len = snprintf( sysdir, sizeof( sysdir ), "/sys/bus/usb/devices/%s", ent->d_name );
    if( sysdir_len < 0 || (size_t)sysdir_len >= sizeof( sysdir ) ) {
      continue;
    }

    uint16_t vid = 0, pid = 0;
    if( read_sysfs_attr_u16( sysdir, "idVendor", &vid ) < 0 ) {
      continue;
    }
    if( read_sysfs_attr_u16( sysdir, "idProduct", &pid ) < 0 ) {
      continue;
    }

    /* Check if this is an RTL2832U device */
    if( vid != SDRGG_USB_VID_REALTEK ) {
      continue;
    }
    if( pid != SDRGG_USB_PID_2832 && pid != SDRGG_USB_PID_2838 ) {
      continue;
    }

    sdrgg_devinfo_t *info = &devs[found];
    memset( info, 0, sizeof( *info ) );
    info->vid = vid;
    info->pid = pid;

    /* Read bus/dev numbers to construct device path */
    char busnum_str[8], devnum_str[8];
    if( read_sysfs_attr_str( sysdir, "busnum", busnum_str, sizeof( busnum_str ) ) < 0 ) {
      continue;
    }
    if( read_sysfs_attr_str( sysdir, "devnum", devnum_str, sizeof( devnum_str ) ) < 0 ) {
      continue;
    }

    int32_t busnum = atoi( busnum_str );
    int32_t devnum = atoi( devnum_str );
    snprintf( info->path, sizeof( info->path ), "/dev/bus/usb/%03d/%03d", busnum, devnum );

    info->bus = (uint8_t)busnum;
    info->devaddr = (uint8_t)devnum;

    read_sysfs_attr_str( sysdir, "serial", info->serial, sizeof( info->serial ) );

    /* Tuner type will be detected on open */
    info->tuner = SDRGG_TUNER_UNKNOWN;

    found++;
  }
  closedir( d );
  return found;
}

/* ---- Utilities ---- */

uint8_t usb::bitrev8( uint8_t byte ) {
  byte = ( ( byte & 0xF0 ) >> 4 ) | ( ( byte & 0x0F ) << 4 );
  byte = ( ( byte & 0xCC ) >> 2 ) | ( ( byte & 0x33 ) << 2 );
  byte = ( ( byte & 0xAA ) >> 1 ) | ( ( byte & 0x55 ) << 1 );
  return byte;
}

uint64_t usb::time_us( void ) {
  struct timeval tv;
  gettimeofday( &tv, NULL );
  return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ---- extern "C" wrapper for sdrgg_enumerate ---- */
extern "C" int32_t sdrgg_enumerate( sdrgg_ctx_t *ctx, sdrgg_devinfo_t *devs, int32_t max_devs ) {
  return usb::enumerate( ctx, devs, max_devs );
}
