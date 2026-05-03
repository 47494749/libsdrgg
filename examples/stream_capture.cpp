#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "../sdrgg.h"

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
}

static uint32_t choose_frequency( const tuner_caps *caps ) {
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

int32_t main( void ) {
  signal( SIGINT, sig_handler );
  signal( SIGTERM, sig_handler );

  sdrgg_ctx_t *ctx = sdr::create();
  if( !ctx ) {
    fprintf( stderr, "failed to create context\n" );
    return 1;
  }

  sdrgg_dev_t *dev = sdr::open( ctx, 0 );
  if( !dev ) {
    fprintf( stderr, "failed to open device 0\n" );
    sdr::destroy( ctx );
    return 1;
  }

  const tuner_caps *caps = nullptr;
  sdr::get_tuner_caps( dev, &caps );
  uint32_t freq_hz = choose_frequency( caps );
  int32_t gain = caps ? caps->total_gain_max_tenth_db * 7 / 10 : 350;

  sdr::set_freq_correction( dev, 0 );
  int32_t rc = sdr::set_sample_rate( dev, 2048000, nullptr );
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "set_sample_rate failed: rc=%d\n", rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    return 1;
  }
  rc = sdr::set_frequency( dev, freq_hz, nullptr );
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "set_frequency(%.3f MHz) failed: rc=%d\n", freq_hz / 1e6, rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    return 1;
  }
  rc = sdr::set_gain( dev, gain );
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "set_gain failed: rc=%d\n", rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    return 1;
  }

  printf( "tuner=%s freq=%.3f MHz gain=%d.%d dB\n",
    caps ? caps->chip_name : "unknown",
    freq_hz / 1e6,
    gain / 10, gain % 10 );

  sdrgg_stream_cfg_t cfg = {
    .buf_count = 32,
    .buf_size = 16384,
  };

  rc = sdr::start_stream( dev, &cfg, stream_callback, nullptr );
  if( rc != SDRGG_OK ) {
    fprintf( stderr, "start_stream failed: rc=%d\n", rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    return 1;
  }

  for( int32_t i = 0; i < 3 && running; i++ ) {
    sleep( 1 );
  }

  sdr::stop_stream( dev );
  printf( "captured %u buffers, %llu bytes\n",
    total_buffers,
    (unsigned long long)total_bytes );

  sdr::close( dev );
  sdr::destroy( ctx );
  return 0;
}