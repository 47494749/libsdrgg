#include <stdio.h>

#include "../sdrgg.h"

static uint32_t choose_frequency( const tuner_caps *caps ) {
  if( !caps ) {
    return 1090000000U;
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
  return caps->freq_min_hz;
}

static uint32_t choose_bandwidth( const tuner_caps *caps ) {
  if( !caps || caps->num_bw_options == 0 || !caps->bw_options ) {
    return 0;
  }

  for( uint8_t i = 0; i < caps->num_bw_options; i++ ) {
    if( caps->bw_options[i].bw_khz >= 3000 ) {
      return caps->bw_options[i].bw_khz;
    }
  }
  return caps->bw_options[0].bw_khz;
}

static int32_t configure_gain( sdrgg_dev_t *dev, sdrgg_tuner_type_t tuner, const tuner_caps *caps ) {
  if( tuner == SDRGG_TUNER_R820T || tuner == SDRGG_TUNER_R820T2 ) {
    int32_t rc = r820t::set_lna_gain( dev, 8 );
    if( rc == SDRGG_OK ) {
      rc = r820t::set_mixer_gain( dev, 6 );
    }
    if( rc == SDRGG_OK ) {
      rc = r820t::set_vga_gain( dev, 10 );
    }
    uint32_t bw_khz = choose_bandwidth( caps );
    if( rc == SDRGG_OK && bw_khz > 0 ) {
      rc = r820t::set_bandwidth( dev, bw_khz );
    }
    return rc;
  }

  if( tuner == SDRGG_TUNER_FC0012 ) {
    const int16_t *gains = nullptr;
    int32_t count = 0;
    int32_t rc = fc0012::get_gains( &gains, &count );
    if( rc != SDRGG_OK || count <= 0 ) {
      return rc;
    }
    return fc0012::set_gain( dev, gains[count - 1] );
  }

  return sdr::set_gain( dev, 350 );
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
  int32_t rc = sdr::get_tuner_caps( dev, &caps );
  if( rc != SDRGG_OK || !caps ) {
    fprintf( stderr, "failed to query tuner caps: rc=%d\n", rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    return 1;
  }

  uint32_t target_freq = choose_frequency( caps );
  uint32_t actual_rate = 0;
  uint32_t actual_freq = 0;

  rc = sdr::set_sample_rate( dev, 2048000, &actual_rate );
  if( rc == SDRGG_OK ) {
    rc = sdr::set_frequency( dev, target_freq, &actual_freq );
  }
  if( rc == SDRGG_OK ) {
    rc = configure_gain( dev, sdr::get_tuner_type( dev ), caps );
  }

  if( rc != SDRGG_OK ) {
    fprintf( stderr, "chip-aware configuration failed: rc=%d\n", rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    return 1;
  }

  printf( "chip=%s implemented=%s\n", caps->chip_name, caps->implemented ? "yes" : "no" );
  printf( "configured sample_rate=%u freq=%u\n", actual_rate, actual_freq );
  printf( "gain stages=%u bw_options=%u mixer_arch=%s\n",
    caps->num_gain_stages,
    caps->num_bw_options,
    caps->mixer_arch == SDRGG_MIXER_ZERO_IF ? "zero-if" : "low-if" );

  sdr::close( dev );
  sdr::destroy( ctx );
  return 0;
}