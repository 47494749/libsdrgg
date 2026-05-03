#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../sdrgg.h"

static volatile int32_t running = 1;
static FILE *output_file = nullptr;
static uint64_t total_bytes = 0;
static uint32_t total_buffers = 0;

static void sig_handler( int32_t sig ) {
  ( void )sig;
  running = 0;
}

static void stream_callback( sdrgg_dev_t *dev, const sdrgg_buffer_t *buf, void *ctx ) {
  ( void )dev;
  ( void )ctx;

  if( output_file ) {
    size_t written = fwrite( buf->data, 1, buf->length, output_file );
    total_bytes += written;
    total_buffers++;
    if( written != buf->length ) {
      running = 0;
    }
  }
}

static uint32_t default_frequency( const tuner_caps *caps ) {
  if( !caps ) {
    return 433920000U;
  }
  if( caps->freq_max_hz >= 1090000000U && caps->freq_min_hz <= 1090000000U ) {
    return 1090000000U;
  }
  if( caps->freq_max_hz >= 868300000U && caps->freq_min_hz <= 868300000U ) {
    return 868300000U;
  }
  if( caps->freq_max_hz >= 433920000U && caps->freq_min_hz <= 433920000U ) {
    return 433920000U;
  }
  return ( caps->freq_min_hz + caps->freq_max_hz ) / 2;
}

int32_t main( int32_t argc, char *argv[] ) {
  const char *path = ( argc > 1 ) ? argv[1] : "capture.u8";
  uint32_t freq_hz_arg = ( argc > 2 ) ? (uint32_t)( atof( argv[2] ) * 1000000.0 ) : 0;
  int32_t seconds = ( argc > 3 ) ? atoi( argv[3] ) : 3;

  signal( SIGINT, sig_handler );
  signal( SIGTERM, sig_handler );

  output_file = fopen( path, "wb" );
  if( !output_file ) {
    perror( "fopen" );
    return 1;
  }

  sdrgg_ctx_t *ctx = sdr::create();
  if( !ctx ) {
    fprintf( stderr, "failed to create context\n" );
    fclose( output_file );
    return 1;
  }

  sdrgg_dev_t *dev = sdr::open( ctx, 0 );
  if( !dev ) {
    fprintf( stderr, "failed to open device 0\n" );
    fclose( output_file );
    sdr::destroy( ctx );
    return 1;
  }

  const tuner_caps *caps = nullptr;
  sdr::get_tuner_caps( dev, &caps );

  uint32_t freq_hz = freq_hz_arg ? freq_hz_arg : default_frequency( caps );
  int32_t gain = caps ? caps->total_gain_max_tenth_db * 7 / 10 : 350;

  /* Validate requested frequency against tuner range */
  if( caps && ( freq_hz < caps->freq_min_hz || freq_hz > caps->freq_max_hz ) ) {
    fprintf( stderr, "frequency %.3f MHz outside tuner range (%.1f-%.1f MHz), using default\n",
      freq_hz / 1e6, caps->freq_min_hz / 1e6, caps->freq_max_hz / 1e6 );
    freq_hz = default_frequency( caps );
  }

  int32_t rc = sdr::set_sample_rate( dev, 2048000, nullptr );
  if( rc == SDRGG_OK ) {
    rc = sdr::set_frequency( dev, freq_hz, nullptr );
  }
  if( rc == SDRGG_OK ) {
    rc = sdr::set_gain( dev, gain );
  }
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "device configuration failed: rc=%d\n", rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    fclose( output_file );
    return 1;
  }

  printf( "tuner=%s freq=%.3f MHz gain=%d.%d dB duration=%ds\n",
    caps ? caps->chip_name : "unknown",
    freq_hz / 1e6,
    gain / 10, gain % 10,
    seconds );

  sdrgg_stream_cfg_t cfg = {
    .buf_count = 32,
    .buf_size = 16384,
  };

  rc = sdr::start_stream( dev, &cfg, stream_callback, nullptr );
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "start_stream failed: rc=%d\n", rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    fclose( output_file );
    return 1;
  }

  for( int32_t i = 0; i < seconds && running; i++ ) {
    sleep( 1 );
  }

  sdr::stop_stream( dev );
  fflush( output_file );
  fclose( output_file );

  printf( "saved %llu bytes in %u buffers to %s\n",
    (unsigned long long)total_bytes,
    total_buffers,
    path );

  sdr::close( dev );
  sdr::destroy( ctx );
  return 0;
}