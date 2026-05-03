#include <stdio.h>
#include <string.h>

#include "../sdrgg.h"

static uint32_t choose_frequency( const tuner_caps *caps ) {
  if( !caps ) {
    return 433920000U;
  }
  /* Pick highest common frequency the tuner can reach */
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

static int32_t choose_gain( const tuner_caps *caps ) {
  if( !caps ) {
    return 350;
  }
  /* Pick ~70% of max gain */
  return caps->total_gain_max_tenth_db * 7 / 10;
}

int32_t main( void ) {
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
  int32_t gain = choose_gain( caps );

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
    return 1;
  }

  printf( "tuner=%s freq=%.3f MHz gain=%d.%d dB\n",
    caps ? caps->chip_name : "unknown",
    freq_hz / 1e6,
    gain / 10, gain % 10 );

  uint8_t buffer[16384];
  uint32_t total_bytes = 0;

  for( int32_t i = 0; i < 8; i++ ) {
    memset( buffer, 0, sizeof( buffer ) );
    rc = sdr::read_sync( dev, buffer, sizeof( buffer ), 1000 );
    if( rc < 0 ) {
      fprintf( stderr, "read_sync failed at iteration %d: rc=%d\n", i, rc );
      sdr::close( dev );
      sdr::destroy( ctx );
      return 1;
    }

    total_bytes += (uint32_t)rc;
    printf( "read_sync[%d]: %d bytes first_iq=(%u,%u)\n",
      i,
      rc,
      buffer[0],
      buffer[1] );
  }

  printf( "total sync bytes read: %u\n", total_bytes );

  sdr::close( dev );
  sdr::destroy( ctx );
  return 0;
}