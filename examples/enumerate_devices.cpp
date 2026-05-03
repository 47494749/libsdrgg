#include <stdio.h>

#include "../sdrgg.h"

static const char *tuner_name( sdrgg_tuner_type_t tuner ) {
  switch( tuner ) {
  case SDRGG_TUNER_R820T: return "R820T";
  case SDRGG_TUNER_R820T2: return "R820T2";
  case SDRGG_TUNER_FC0012: return "FC0012";
  case SDRGG_TUNER_FC0013: return "FC0013";
  case SDRGG_TUNER_FC2580: return "FC2580";
  case SDRGG_TUNER_E4000: return "E4000";
  default: return "UNKNOWN";
  }
}

int32_t main( void ) {
  sdrgg_ctx_t *ctx = sdr::create();
  if( !ctx ) {
    fprintf( stderr, "failed to create libsdrgg context\n" );
    return 1;
  }

  sdrgg_devinfo_t devs[8] = {};
  int32_t count = sdrgg_enumerate( ctx, devs, 8 );
  if( count < 0 ) {
    fprintf( stderr, "enumeration failed: rc=%d\n", count );
    sdr::destroy( ctx );
    return 1;
  }

  printf( "found %d device(s)\n", count );
  for( int32_t i = 0; i < count; i++ ) {
    printf( "[%d] path=%s vid=%04X pid=%04X serial=%s tuner=%s\n",
      i,
      devs[i].path,
      devs[i].vid,
      devs[i].pid,
      devs[i].serial,
      tuner_name( devs[i].tuner ) );
  }

  sdr::destroy( ctx );
  return 0;
}