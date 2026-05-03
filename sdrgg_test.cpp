/*
* sdrgg_test.c - Basic test/demo for libsdrgg
*
* Enumerates devices, opens first one, tunes to a frequency,
* captures a few seconds of IQ samples, prints stats.
*
* Usage: ./sdrgg_test [freq_mhz] [seconds]
*   Default: 868.3 MHz, 3 seconds
*
* License: MIT
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "sdrgg.h"

static volatile int32_t running = 1;
static uint64_t total_bytes = 0;
static uint32_t total_buffers = 0;

static void sig_handler( int32_t sig ) {
  ( void )sig;
  running = 0;
}

static void stream_callback( sdrgg_dev_t *dev, const sdrgg_buffer_t *buf, void *ctx ) {
  ( void )dev;
  ( void )ctx;
  total_bytes += buf->length;
  total_buffers++;

  if( total_buffers % 100 == 0 ) {
    fprintf( stderr, "\r  buffers=%u  bytes=%llu  seq=%u", total_buffers, ( unsigned long long )total_bytes, buf->sequence );
  }
}

static const char *tuner_name( sdrgg_tuner_type_t t ) {
  switch( t ) {
  case SDRGG_TUNER_R820T: return "R820T";
  case SDRGG_TUNER_R820T2: return "R820T2";
  case SDRGG_TUNER_FC0012: return "FC0012";
  case SDRGG_TUNER_E4000: return "E4000";
  default: return "Unknown";
  }
}

int32_t main( int32_t argc, char *argv[] ) {
  uint32_t freq_hz = 868300000; /* 868.3 MHz default */
  int32_t duration_s = 3;
  int32_t dev_index = 0;

  if( argc > 1 ) {
    freq_hz = (uint32_t)( atof( argv[1] ) * 1000000.0 );
  }
  if( argc > 2 ) {
    duration_s = atoi( argv[2] );
  }
  if( argc > 3 ) {
    dev_index = atoi( argv[3] );
  }

  signal( SIGINT, sig_handler );
  signal( SIGTERM, sig_handler );

  printf( "libsdrgg test utility\n" );
  printf( "  target freq: %.3f MHz\n", freq_hz / 1e6 );
  printf( "  duration: %d seconds\n\n", duration_s );

  /* Create context */
  sdrgg_ctx_t *ctx = sdr::create();
  if( !ctx ) {
    fprintf( stderr, "Failed to create context\n" );
    return 1;
  }

  /* Enumerate devices */
  sdrgg_devinfo_t devs[8];
  int32_t count = sdrgg_enumerate( ctx, devs, 8 );
  printf( "Found %d device(s):\n", count );

  for( int32_t i = 0; i < count; i++ ) {
    printf( "  [%d] %s  VID=%04X PID=%04X  serial=%s\n", i, devs[i].path, devs[i].vid, devs[i].pid, devs[i].serial );
  }

  if( count == 0 ) {
    fprintf( stderr, "No RTL2832U devices found.\n" );
    sdr::destroy( ctx );
    return 1;
  }

  /* Open specified device */
  printf( "\nOpening device %d...\n", dev_index );
  sdrgg_dev_t *dev = sdr::open( ctx, dev_index );
  if( !dev ) {
    fprintf( stderr, "Failed to open device (permission? try: sudo)\n" );
    sdr::destroy( ctx );
    return 1;
  }

  printf( "  Tuner: %s\n", tuner_name( sdr::get_tuner_type( dev ) ) );
  printf( "  Crystal: %u Hz\n", sdr::get_xtal_freq( dev ) );

  /* Configure */
  sdr::set_freq_correction( dev, 0 );
  sdr::set_sample_rate( dev, 1600000, NULL );

  uint32_t actual_freq = 0;
  int32_t rc = sdr::set_frequency( dev, freq_hz, &actual_freq );
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "Failed to set frequency (rc=%d)\n", rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    return 1;
  }
  printf( "  Frequency set: %.3f MHz\n", actual_freq / 1e6 );

  sdr::set_gain( dev, 350 ); /* 35.0 dB */
  printf( "  Gain: 35.0 dB (manual)\n" );

  /* Direct register access demo */
  printf( "\n  -- Direct register read demo --\n" );
  uint8_t lna_idx = 0, mix_idx = 0;
  r820t::read_signal( dev, &lna_idx, &mix_idx );
  printf( "  R820T signal: LNA_idx=%u  Mixer_idx=%u\n", lna_idx, mix_idx );

  /* Read raw register 0x05 */
  uint8_t reg05;
  tuner::read_reg( dev, 0x05, &reg05 );
  printf( "  R820T R05 = 0x%02X\n", reg05 );

  /* Start streaming */
  printf( "\nStarting stream (%d sec)...\n", duration_s );
  sdrgg_stream_cfg_t scfg = {
    .buf_count = 32,
    .buf_size = 16384,
  };

  rc = sdr::start_stream( dev, &scfg, stream_callback, NULL );
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "Failed to start stream (rc=%d)\n", rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    return 1;
  }

  /* Run for specified duration */
  for( int32_t i = 0; i < duration_s && running; i++ ) {
    sleep( 1 );
  }

  sdr::stop_stream( dev );

  printf( "\n\nDone. Captured %llu bytes in %u buffers (%.2f MB/s)\n", ( unsigned long long )total_bytes, total_buffers, (double)total_bytes / ( duration_s * 1024.0 * 1024.0 ) );

  /* Cleanup */
  sdr::close( dev );
  sdr::destroy( ctx );

  return 0;
}
