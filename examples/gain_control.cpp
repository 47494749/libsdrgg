#include <stdio.h>

#include "../sdrgg.h"

static const char *tuner_name( sdrgg_tuner_type_t tuner ) {
  switch( tuner ) {
  case SDRGG_TUNER_R820T: return "R820T";
  case SDRGG_TUNER_R820T2: return "R820T2";
  case SDRGG_TUNER_FC0012: return "FC0012";
  default: return "UNSUPPORTED";
  }
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

  sdrgg_tuner_type_t tuner = sdr::get_tuner_type( dev );
  printf( "tuner=%s\n", tuner_name( tuner ) );

  int32_t rc = SDRGG_OK;
  if( tuner == SDRGG_TUNER_R820T || tuner == SDRGG_TUNER_R820T2 ) {
    rc = r820t::set_lna_gain( dev, 8 );
    if( rc == SDRGG_OK ) {
      rc = r820t::set_mixer_gain( dev, 6 );
    }
    if( rc == SDRGG_OK ) {
      rc = r820t::set_vga_gain( dev, 10 );
    }
    if( rc == SDRGG_OK ) {
      rc = r820t::set_bandwidth( dev, 3000 );
    }

    if( rc == SDRGG_OK ) {
      uint8_t lna_idx = 0;
      uint8_t mixer_idx = 0;
      r820t::read_signal( dev, &lna_idx, &mixer_idx );
      printf( "manual R820T gain set: lna_idx=%u mixer_idx=%u vga_idx=%d bw=%u kHz\n",
        lna_idx,
        mixer_idx,
        10,
        3000U );
    }
  } else if( tuner == SDRGG_TUNER_FC0012 ) {
    const int16_t *gains = nullptr;
    int32_t count = 0;
    rc = fc0012::get_gains( &gains, &count );
    if( rc == SDRGG_OK ) {
      printf( "available FC0012 gains (0.1 dB):" );
      for( int32_t i = 0; i < count; i++ ) {
        printf( " %d", gains[i] );
      }
      printf( "\n" );
    }
    if( rc == SDRGG_OK && count > 0 ) {
      int32_t selected = gains[count / 2];
      rc = fc0012::set_gain( dev, selected );
      if( rc == SDRGG_OK ) {
        printf( "selected FC0012 gain = %d (0.1 dB)\n", selected );
      }
    }
  } else {
    fprintf( stderr, "this example currently supports only R820T/R820T2 and FC0012\n" );
    rc = SDRGG_ERR_NODEV;
  }

  if( rc != SDRGG_OK ) {
    fprintf( stderr, "gain-control example failed: rc=%d\n", rc );
    sdr::close( dev );
    sdr::destroy( ctx );
    return 1;
  }

  sdr::close( dev );
  sdr::destroy( ctx );
  return 0;
}