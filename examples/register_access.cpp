#include <stdio.h>

#include "../sdrgg.h"

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

  if( tuner == SDRGG_TUNER_R820T || tuner == SDRGG_TUNER_R820T2 ) {
    printf( "tuner: R820T/R820T2\n" );

    uint8_t reg = 0;
    if( tuner::read_reg( dev, 0x05, &reg ) == SDRGG_OK ) {
      printf( "  reg[0x05] = 0x%02X\n", reg );
    }

    uint8_t lna_idx = 0;
    uint8_t mixer_idx = 0;
    if( r820t::read_signal( dev, &lna_idx, &mixer_idx ) == SDRGG_OK ) {
      printf( "  signal indices: lna=%u mixer=%u\n", lna_idx, mixer_idx );
    }

    bool locked = false;
    if( r820t::pll_locked( dev, &locked ) == SDRGG_OK ) {
      printf( "  pll_locked=%s\n", locked ? "yes" : "no" );
    }
  } else if( tuner == SDRGG_TUNER_FC0012 ) {
    printf( "tuner: FC0012\n" );

    /* FC0012 chip ID register (0x00) */
    uint8_t chip_id = 0;
    if( tuner::read_reg( dev, 0x00, &chip_id ) == SDRGG_OK ) {
      printf( "  chip_id[0x00] = 0x%02X (expect 0xA1)\n", chip_id );
    }

    /* FC0012 LNA gain register */
    uint8_t lna = 0;
    if( tuner::read_reg( dev, 0x13, &lna ) == SDRGG_OK ) {
      printf( "  lna_gain[0x13] = 0x%02X\n", lna );
    }

    /* FC0012 VCO calibration register */
    uint8_t vco = 0;
    if( tuner::read_reg( dev, 0x0E, &vco ) == SDRGG_OK ) {
      printf( "  vco_cal[0x0E] = 0x%02X\n", vco );
    }
  } else {
    printf( "tuner: unknown type %d\n", (int)tuner );
    uint8_t reg = 0;
    if( tuner::read_reg( dev, 0x00, &reg ) == SDRGG_OK ) {
      printf( "  reg[0x00] = 0x%02X\n", reg );
    }
  }

  sdr::close( dev );
  sdr::destroy( ctx );
  return 0;
}