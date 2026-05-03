#include <stdio.h>

#include "../sdrgg.h"

static void print_caps( const tuner_caps *caps ) {
  if( !caps ) {
    return;
  }

  printf( "chip=%s implemented=%s\n", caps->chip_name, caps->implemented ? "yes" : "no" );
  printf( "  freq range: %u .. %u Hz\n", caps->freq_min_hz, caps->freq_max_hz );
  printf( "  i2c addr: 0x%02X writable_regs=%u xtal=%u Hz\n",
    caps->i2c_addr,
    caps->i2c_reg_count,
    caps->xtal_freq_hz );
  printf( "  mixer arch: %s if_freq=%u Hz pll_step=%u fractional_pll=%s\n",
    caps->mixer_arch == SDRGG_MIXER_ZERO_IF ? "zero-if" : "low-if",
    caps->if_freq_hz,
    caps->pll_step_hz,
    caps->has_fractional_pll ? "yes" : "no" );
  printf( "  gain stages: %u total_gain=%d .. %d (0.1 dB) agc=%s separate_lna=%s\n",
    caps->num_gain_stages,
    caps->total_gain_min_tenth_db,
    caps->total_gain_max_tenth_db,
    caps->has_agc ? "yes" : "no",
    caps->has_separate_lna_control ? "yes" : "no" );

  for( uint8_t i = 0; i < caps->num_gain_stages; i++ ) {
    const gain_stage_info *stage = &caps->gain_stages[i];
    printf( "    stage[%u]: %s range=%d .. %d steps=%u\n",
      i,
      stage->name,
      stage->min_tenth_db,
      stage->max_tenth_db,
      stage->num_steps );
  }

  if( caps->num_freq_gaps > 0 ) {
    printf( "  frequency gaps:\n" );
    for( uint8_t i = 0; i < caps->num_freq_gaps; i++ ) {
      printf( "    %u .. %u Hz\n", caps->freq_gaps[i].start_hz, caps->freq_gaps[i].stop_hz );
    }
  }

  if( caps->num_bw_options > 0 ) {
    printf( "  bandwidth options (kHz):" );
    for( uint8_t i = 0; i < caps->num_bw_options; i++ ) {
      printf( " %u", caps->bw_options[i].bw_khz );
    }
    printf( "\n" );
  }
}

int32_t main( void ) {
  const sdrgg_tuner_type_t tuners[] = {
    SDRGG_TUNER_R820T,
    SDRGG_TUNER_R820T2,
    SDRGG_TUNER_FC0012,
    SDRGG_TUNER_FC0013,
    SDRGG_TUNER_FC2580,
    SDRGG_TUNER_E4000,
  };

  for( uint32_t i = 0; i < sizeof( tuners ) / sizeof( tuners[0] ); i++ ) {
    print_caps( sdr::get_caps_by_type( tuners[i] ) );
    printf( "\n" );
  }

  return 0;
}