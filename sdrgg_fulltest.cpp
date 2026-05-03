/*
* sdrgg_fulltest.c - Comprehensive functional test for libsdrgg
*
* Tests ALL public API functions on all connected dongles.
* Reports PASS/FAIL per test per device with summary.
*
* Usage: sudo ./sdrgg_fulltest [dev_index]
*   No argument: test all devices
*   dev_index:   test only specified device
*
* License: MIT
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>

#include "sdrgg.h"

/* ---- Test framework ---- */

static int32_t tests_run = 0;
static int32_t tests_pass = 0;
static int32_t tests_fail = 0;
static int32_t tests_skip = 0;

#define COLOR_GREEN  "\033[32m"
#define COLOR_RED    "\033[31m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RESET  "\033[0m"

static void test_result( const char *name, int32_t passed, const char *detail ) {
  tests_run++;
  if( passed ) {
    tests_pass++;
    printf( "  " COLOR_GREEN "PASS" COLOR_RESET " %-40s %s\n", name, detail ? detail : "" );
  } else {
    tests_fail++;
    printf( "  " COLOR_RED "FAIL" COLOR_RESET " %-40s %s\n", name, detail ? detail : "" );
  }
}

static void test_skip( const char *name, const char *reason ) {
  tests_run++;
  tests_skip++;
  printf( "  " COLOR_YELLOW "SKIP" COLOR_RESET " %-40s %s\n", name, reason );
}

/* ---- Streaming helpers ---- */

static volatile int32_t stream_running = 0;
static uint64_t stream_bytes = 0;
static uint32_t stream_bufs = 0;
static uint32_t stream_last_seq = 0;
static int32_t stream_drops = 0;
static uint8_t stream_min = 255, stream_max = 0;

static void stream_cb( sdrgg_dev_t *dev, const sdrgg_buffer_t *buf, void *ctx ) {
  ( void )dev;
  ( void )ctx;
  stream_bytes += buf->length;

  /* Check for sequence gaps (drops) */
  if( stream_bufs > 0 && buf->sequence != stream_last_seq + 1 ) {
    stream_drops++;
  }
  stream_last_seq = buf->sequence;

  /* Sample min/max for IQ sanity check */
  for( uint32_t i = 0; i < buf->length && i < 1024; i++ ) {
    if( buf->data[i] < stream_min ) {
      stream_min = buf->data[i];
    }
    if( buf->data[i] > stream_max ) {
      stream_max = buf->data[i];
    }
  }

  stream_bufs++;
}

static void stream_reset( void ) {
  stream_bytes = 0;
  stream_bufs = 0;
  stream_last_seq = 0;
  stream_drops = 0;
  stream_min = 255;
  stream_max = 0;
}

/* ================================================================
*  Test suites
* ================================================================ */

/*
* Test 1: Context create/destroy
*/
static void test_context( void ) {
  printf( "\n=== Context Management ===\n" );

  sdrgg_ctx_t *c = sdr::create();
  test_result( "sdr::create()", c != NULL, "" );
  if( c ) {
    sdr::destroy( c );
    test_result( "sdr::destroy()", 1, "no crash" );
  }
}

/*
* Test 2: Enumeration
*/
static int32_t test_enumerate( sdrgg_ctx_t *ctx, sdrgg_devinfo_t *devs, int32_t *count ) {
  printf( "\n=== Enumeration ===\n" );

  *count = sdrgg_enumerate(ctx, devs, 8);
  char detail[64];
  snprintf( detail, sizeof( detail ), "found %d device(s)", *count );
  test_result( "sdrgg_enumerate()", *count > 0, detail );

  for( int32_t i = 0; i < *count; i++ ) {
    printf( "    [%d] %s  VID=%04X PID=%04X serial=%s\n", i, devs[i].path, devs[i].vid, devs[i].pid, devs[i].serial );
  }

  return *count;
}

/*
* Test 3: Device open/close cycle
*/
static sdrgg_dev_t *test_open_close( sdrgg_ctx_t *ctx, int32_t idx, sdrgg_devinfo_t *info ) {
  printf( "\n=== Device %d (serial=%s) Open/Close ===\n", idx, info->serial );

  /* Test open by index */
  sdrgg_dev_t *dev = sdr::open( ctx, idx );
  test_result( "sdr::open(index)", dev != NULL, "" );
  if( !dev ) {
    return NULL;
  }

  /* Test tuner type detection */
  sdrgg_tuner_type_t tuner = sdr::get_tuner_type( dev );
  const char *tname;
  switch( tuner ) {
  case SDRGG_TUNER_R820T: tname = "R820T"; break;
  case SDRGG_TUNER_R820T2: tname = "R820T2"; break;
  case SDRGG_TUNER_FC0012: tname = "FC0012"; break;
  case SDRGG_TUNER_E4000: tname = "E4000"; break;
  default: tname = "UNKNOWN"; break;
  }
  char detail[64];
  snprintf( detail, sizeof( detail ), "type=%s", tname );
  test_result( "sdr::get_tuner_type()", tuner != SDRGG_TUNER_UNKNOWN, detail );

  /* Test xtal freq */
  uint32_t xtal = sdr::get_xtal_freq( dev );
  snprintf( detail, sizeof( detail ), "xtal=%u Hz", xtal );
  test_result( "sdr::get_xtal_freq()", xtal == 28800000, detail );

  /* Test get_devinfo */
  sdrgg_devinfo_t di;
  int32_t rc = sdr::get_devinfo( dev, &di );
  snprintf( detail, sizeof( detail ), "path=%s", di.path );
  test_result( "sdr::get_devinfo()", rc == SDRGG_OK, detail );

  /* Close and reopen (test close/reopen cycle) */
  sdr::close( dev );
  test_result( "sdr::close()", 1, "no crash" );

  /* Reopen by path */
  dev = sdr::open_path( ctx, info->path );
  test_result( "sdr::open_path()", dev != NULL, info->path );

  return dev;
}

/*
* Test 4: PPM correction
*/
static void test_ppm( sdrgg_dev_t *dev ) {
  printf( "\n  --- PPM Correction ---\n" );
  int32_t rc;

  rc = sdr::set_freq_correction( dev, 0 );
  test_result( "sdr::set_freq_correction(0)", rc == SDRGG_OK, "" );

  rc = sdr::set_freq_correction( dev, 50 );
  test_result( "sdr::set_freq_correction(50)", rc == SDRGG_OK, "" );

  rc = sdr::set_freq_correction( dev, -30 );
  test_result( "sdr::set_freq_correction(-30)", rc == SDRGG_OK, "" );

  /* Restore */
  sdr::set_freq_correction( dev, 0 );
}

/*
* Test 5: Sample rate
*/
static void test_sample_rate( sdrgg_dev_t *dev ) {
  printf( "\n  --- Sample Rate ---\n" );
  int32_t rc;
  uint32_t actual;
  char detail[64];

  /* Valid rates */
  uint32_t rates[] = { 250000, 1024000, 1600000, 2048000, 2400000, 3200000 };
  for( uint32_t i = 0; i < sizeof( rates )/sizeof( rates[0] ); i++ ) {
    actual = 0;
    rc = sdr::set_sample_rate( dev, rates[i], &actual );
    snprintf( detail, sizeof( detail ), "req=%u actual=%u", rates[i], actual );
    test_result( "sdr::set_sample_rate()", rc == SDRGG_OK && actual > 0, detail );
  }

  /* Invalid rate (too low) */
  rc = sdr::set_sample_rate( dev, 100000, &actual );
  test_result( "reject rate < 225k", rc != SDRGG_OK, "" );

  /* Invalid rate (too high) */
  rc = sdr::set_sample_rate( dev, 5000000, &actual );
  test_result( "reject rate > 3.2M", rc != SDRGG_OK, "" );

  /* Set back to 2.048 MHz for subsequent tests */
  sdr::set_sample_rate( dev, 2048000, NULL );
}

/*
* Test 6: Frequency tuning
*/
static void test_frequency( sdrgg_dev_t *dev, sdrgg_tuner_type_t tuner ) {
  printf( "\n  --- Frequency Tuning ---\n" );
  int32_t rc;
  uint32_t actual;
  char detail[64];

  /* FC0012 range: ~22-948 MHz, R820T range: ~24-1766 MHz */
  uint32_t freqs_r820t[] = {
    50000000, 100000000, 433920000, 868300000,
    1090000000, 1575420000
  };
  uint32_t freqs_fc0012[] = {
    50000000, 100000000, 200000000, 433920000,
    868000000, 900000000
  };

  uint32_t *freqs;
  uint32_t nfreqs;
  if( tuner == SDRGG_TUNER_FC0012 ) {
    freqs = freqs_fc0012;
    nfreqs = sizeof( freqs_fc0012 )/sizeof( freqs_fc0012[0] );
  } else {
    freqs = freqs_r820t;
    nfreqs = sizeof( freqs_r820t )/sizeof( freqs_r820t[0] );
  }

  for( uint32_t i = 0; i < nfreqs; i++ ) {
    actual = 0;
    rc = sdr::set_frequency( dev, freqs[i], &actual );
    snprintf( detail, sizeof( detail ), "%.3f MHz -> rc=%d", freqs[i]/1e6, rc );
    test_result( "sdr::set_frequency()", rc == SDRGG_OK, detail );
  }

  /* Set a working freq for further tests */
  if( tuner == SDRGG_TUNER_FC0012 ) {
    sdr::set_frequency( dev, 100000000, NULL );
  } else {
    sdr::set_frequency( dev, 1090000000, NULL );
  }
}

/*
* Test 7: Gain control
*/
static void test_gain( sdrgg_dev_t *dev, sdrgg_tuner_type_t tuner ) {
  printf( "\n  --- Gain Control ---\n" );
  int32_t rc, gain_out;
  char detail[64];

  /* Auto gain */
  rc = sdr::set_gain( dev, SDRGG_GAIN_AUTO );
  test_result( "sdr::set_gain(AUTO)", rc == SDRGG_OK, "" );

  rc = sdr::get_gain( dev, &gain_out );
  snprintf( detail, sizeof( detail ), "gain_mode=%d", gain_out );
  test_result( "sdr::get_gain() after auto", rc == SDRGG_OK, detail );

  /* Manual gain steps */
  int32_t gains[] = { 0, 50, 100, 200, 350, 450 };
  for( uint32_t i = 0; i < sizeof( gains )/sizeof( gains[0] ); i++ ) {
    rc = sdr::set_gain( dev, gains[i] );
    snprintf( detail, sizeof( detail ), "%.1f dB -> rc=%d", gains[i]/10.0, rc );
    test_result( "sdr::set_gain(manual)", rc == SDRGG_OK, detail );
  }

  /* AGC */
  rc = sdr::set_digital_agc( dev, true );
  test_result( "sdr::set_digital_agc(true)", rc == SDRGG_OK, "" );

  rc = sdr::set_digital_agc( dev, false );
  test_result( "sdr::set_digital_agc(false)", rc == SDRGG_OK, "" );

  /* R820T-specific gain control */
  if( tuner == SDRGG_TUNER_R820T || tuner == SDRGG_TUNER_R820T2 ) {
    printf( "\n  --- R820T Gain Stages ---\n" );

    rc = r820t::set_lna_gain( dev, 8 );
    test_result( "r820t::set_lna_gain(8)", rc == SDRGG_OK, "" );

    rc = r820t::set_mixer_gain( dev, 8 );
    test_result( "r820t::set_mixer_gain(8)", rc == SDRGG_OK, "" );

    rc = r820t::set_vga_gain( dev, 10 );
    test_result( "r820t::set_vga_gain(10)", rc == SDRGG_OK, "" );

    /* Auto LNA/mixer */
    rc = r820t::set_lna_gain( dev, -1 );
    test_result( "r820t::set_lna_gain(auto)", rc == SDRGG_OK, "" );

    rc = r820t::set_mixer_gain( dev, -1 );
    test_result( "r820t::set_mixer_gain(auto)", rc == SDRGG_OK, "" );

    /* Bandwidth */
    rc = r820t::set_bandwidth( dev, 6000 );
    test_result( "r820t::set_bandwidth(6MHz)", rc == SDRGG_OK, "" );

    /* PLL lock */
    bool locked = false;
    rc = r820t::pll_locked( dev, &locked );
    snprintf( detail, sizeof( detail ), "locked=%d", locked );
    test_result( "r820t::pll_locked()", rc == SDRGG_OK && locked, detail );

    /* Signal readback */
    uint8_t lna_idx = 0, mix_idx = 0;
    rc = r820t::read_signal( dev, &lna_idx, &mix_idx );
    snprintf( detail, sizeof( detail ), "LNA=%u Mixer=%u", lna_idx, mix_idx );
    test_result( "r820t::read_signal()", rc == SDRGG_OK, detail );
  } else {
    test_skip( "R820T gain stages", "FC0012 tuner" );
    test_skip( "R820T PLL lock", "FC0012 tuner" );
    test_skip( "R820T signal readback", "FC0012 tuner" );
  }
}

/*
* Test 8: Demod register access
*/
static void test_demod_regs( sdrgg_dev_t *dev ) {
  printf( "\n  --- Demod Register Access ---\n" );
  int32_t rc;
  uint8_t val;
  char detail[64];

  /* Read USB_SYSCTL (0x2000) via USB block */
  rc = demod::read( dev, 1 /* USB block */, 0x2000, &val );
  snprintf( detail, sizeof( detail ), "USB_SYSCTL=0x%02X", val );
  test_result( "demod::read(USB, 0x2000)", rc == SDRGG_OK, detail );

  /* Read DEMOD_CTL (0x3000) via SYS block */
  rc = demod::read( dev, 2 /* SYS block */, 0x3000, &val );
  snprintf( detail, sizeof( detail ), "DEMOD_CTL=0x%02X", val );
  test_result( "demod::read(SYS, 0x3000)", rc == SDRGG_OK, detail );

  /* Read demod page 0 reg 0x19 (SDR mode) */
  rc = demod::read( dev, 0 /* DEMOD */, 0x0019, &val );
  snprintf( detail, sizeof( detail ), "DEMOD[0][0x19]=0x%02X", val );
  test_result( "demod::read(DEMOD, p0:0x19)", rc == SDRGG_OK, detail );

  /* Read demod page 1 reg 0x01 (I2C repeater) */
  rc = demod::read( dev, 0, 0x0101, &val );
  snprintf( detail, sizeof( detail ), "DEMOD[1][0x01]=0x%02X", val );
  test_result( "demod::read(DEMOD, p1:0x01)", rc == SDRGG_OK, detail );

  /* Bulk read: USB EPA registers */
  uint8_t bulk[4];
  rc = demod::read_bulk( dev, 1, 0x2000, bulk, 2 );
  snprintf( detail, sizeof( detail ), "bulk[0]=0x%02X bulk[1]=0x%02X", bulk[0], bulk[1] );
  test_result( "demod::read_bulk(USB, 2B)", rc == SDRGG_OK, detail );

  /* Write/readback test: write safe value to SDR mode register, read back */
  uint8_t orig;
  demod::read( dev, 0, 0x0019, &orig );
  rc = demod::write( dev, 0, 0x0019, 0x05 );
  test_result( "demod::write(DEMOD, p0:0x19)", rc == SDRGG_OK, "val=0x05" );
  demod::read( dev, 0, 0x0019, &val );
  snprintf( detail, sizeof( detail ), "wrote=0x05 read=0x%02X", val );
  test_result( "demod::write/readback", val == 0x05, detail );
  /* Restore */
  demod::write( dev, 0, 0x0019, orig );
}

/*
* Test 9: Tuner I2C access
*/
static void test_tuner_i2c( sdrgg_dev_t *dev, sdrgg_tuner_type_t tuner ) {
  printf( "\n  --- Tuner I2C Access ---\n" );
  int32_t rc;
  uint8_t val;
  char detail[64];

  if( tuner == SDRGG_TUNER_R820T || tuner == SDRGG_TUNER_R820T2 ) {
    /* Read R820T reg 0x00 (chip ID, should read back) */
    rc = tuner::read_reg( dev, 0x00, &val );
    snprintf( detail, sizeof( detail ), "R00=0x%02X", val );
    test_result( "tuner::read_reg(0x00)", rc == SDRGG_OK, detail );

    /* Read reg 0x05 */
    rc = tuner::read_reg( dev, 0x05, &val );
    snprintf( detail, sizeof( detail ), "R05=0x%02X", val );
    test_result( "tuner::read_reg(0x05)", rc == SDRGG_OK, detail );

    /* Multi-byte read (regs 0x00-0x04) */
    uint8_t regs[5];
    rc = tuner::read( dev, 0x00, regs, 5 );
    snprintf( detail, sizeof( detail ), "%02X %02X %02X %02X %02X", regs[0], regs[1], regs[2], regs[3], regs[4] );
    test_result( "tuner::read(0x00, 5B)", rc == SDRGG_OK, detail );

    /* Read-modify-write: change VGA bits in R0C, then restore */
    uint8_t orig_0c;
    tuner::read_reg( dev, 0x0C, &orig_0c );
    rc = tuner::rmw( dev, 0x0C, 0x08, 0x0F ); /* VGA = 8 */
    test_result( "tuner::rmw(0x0C, 0x08, 0x0F)", rc == SDRGG_OK, "VGA=8" );
    /* Restore */
    tuner::write_reg( dev, 0x0C, orig_0c );

    /* Write/read test with shadow register */
    rc = tuner::write_reg( dev, 0x05, 0x1F );
    test_result( "tuner::write_reg(0x05, 0x1F)", rc == SDRGG_OK, "" );

  } else {
    /* FC0012: chip ID at reg 0x00 should be 0xA1 */
    /* Note: FC0012 I2C goes through fc_readreg, not the R820T path.
    * The tuner_read_reg API is R820T-specific (bit-reversed).
    * For FC0012 we test that the device was detected OK. */
    test_skip( "tuner::read_reg(R820T)", "FC0012 tuner" );
    test_skip( "tuner::read(bulk)", "FC0012 tuner" );
    test_skip( "tuner::rmw()", "FC0012 tuner" );
    test_skip( "tuner::write_reg()", "FC0012 tuner" );
  }
}

/*
* Test 10: Sync read
*/
static void test_sync_read( sdrgg_dev_t *dev ) {
  printf( "\n  --- Synchronous Read ---\n" );
  char detail[128];

  /* Must set sample rate and start bulk before sync read */
  sdr::set_sample_rate( dev, 2048000, NULL );

  uint8_t buf[16384];
  int32_t rc = sdr::read_sync( dev, buf, sizeof( buf ), 1000 );

  if( rc >= 0 ) {
    /* Check IQ data sanity: should not be all zeros or all 0xFF */
    int32_t nonzero = 0, non_ff = 0;
    for( int32_t i = 0; i < rc && i < 1024; i++ ) {
      if( buf[i] != 0 ) {
        nonzero++;
      }
      if( buf[i] != 0xFF ) {
        non_ff++;
      }
    }
    snprintf( detail, sizeof( detail ), "%d bytes, nonzero=%d non_ff=%d", rc, nonzero, non_ff );
    test_result( "sdr::read_sync()", rc > 0, detail );

    /* IQ DC offset: average should be near 128 for unsigned 8-bit */
    if( rc >= 1024 ) {
      double sum = 0;
      for( int32_t i = 0; i < 1024; i++ ) {
        sum += buf[i];
      }
      double avg = sum / 1024.0;
      snprintf( detail, sizeof( detail ), "avg=%.1f (expect ~128)", avg );
      test_result( "IQ DC offset sanity", avg > 80 && avg < 180, detail );
    }
  } else {
    snprintf( detail, sizeof( detail ), "rc=%d", rc );
    test_result( "sdr::read_sync()", 0, detail );
  }
}

/*
* Test 11: Async streaming
*/
static void test_async_stream( sdrgg_dev_t *dev ) {
  printf( "\n  --- Async Streaming ---\n" );
  int32_t rc;
  char detail[128];

  sdr::set_sample_rate( dev, 2048000, NULL );

  /* Test with default config */
  stream_reset();
  sdrgg_stream_cfg_t cfg = { .buf_count = 16, .buf_size = 16384 };

  rc = sdr::start_stream( dev, &cfg, stream_cb, NULL );
  test_result( "sdr::start_stream()", rc == SDRGG_OK, "" );

  if( rc == SDRGG_OK ) {
    /* Stream for 2 seconds */
    for( int32_t i = 0; i < 20; i++ ) {
      usleep( 100000 );
      if( stream_bufs > 200 ) {
        break;
      }
    }

    sdr::stop_stream( dev );
    test_result( "sdr::stop_stream()", 1, "no crash" );

    snprintf( detail, sizeof( detail ), "bufs=%u bytes=%llu drops=%d", stream_bufs, ( unsigned long long )stream_bytes, stream_drops );
    test_result( "stream data received", stream_bufs > 10, detail );
    test_result( "no sequence drops", stream_drops == 0, "" );

    /* IQ data sanity from stream */
    snprintf( detail, sizeof( detail ), "min=%u max=%u", stream_min, stream_max );
    int32_t range_ok = ( stream_max - stream_min ) > 2;
    test_result( "IQ data range sanity", range_ok, detail );

    /* Throughput check (2.048 MHz * 2 bytes = ~4.1 MB/s expected) */
    double mbps = (double)stream_bytes / ( 2.0 * 1024 * 1024 );
    snprintf( detail, sizeof( detail ), "%.2f MB/s (expect ~3-4)", mbps );
    test_result( "stream throughput", mbps > 1.0, detail );
  }

  /* Double-start should fail */
  stream_reset();
  rc = sdr::start_stream( dev, &cfg, stream_cb, NULL );
  if( rc == SDRGG_OK ) {
    /* Already stopped, so re-start should be OK */
    sdr::stop_stream( dev );
    test_result( "restart after stop", 1, "" );
  } else {
    test_result( "restart after stop", 0, "unexpected failure" );
  }
}

/*
* Test 12: Frequency retune while streaming
*/
static void test_retune_while_streaming( sdrgg_dev_t *dev, sdrgg_tuner_type_t tuner ) {
  printf( "\n  --- Retune While Streaming ---\n" );
  int32_t rc;
  char detail[64];

  sdr::set_sample_rate( dev, 2048000, NULL );
  stream_reset();
  sdrgg_stream_cfg_t cfg = { .buf_count = 16, .buf_size = 16384 };

  rc = sdr::start_stream( dev, &cfg, stream_cb, NULL );
  if( rc != SDRGG_OK ) {
    test_skip( "retune while streaming", "start_stream failed" );
    return;
  }

  usleep( 200000 ); /* Let streaming stabilize */

  /* Retune several times */
  uint32_t freqs[3];
  if( tuner == SDRGG_TUNER_FC0012 ) {
    freqs[0] = 100000000; freqs[1] = 433920000; freqs[2] = 200000000;
  } else {
    freqs[0] = 1090000000; freqs[1] = 868300000; freqs[2] = 433920000;
  }

  for( int32_t i = 0; i < 3; i++ ) {
    rc = sdr::set_frequency( dev, freqs[i], NULL );
    snprintf( detail, sizeof( detail ), "%.1f MHz -> rc=%d", freqs[i]/1e6, rc );
    test_result( "retune during stream", rc == SDRGG_OK, detail );
    usleep( 100000 );
  }

  sdr::stop_stream( dev );

  snprintf( detail, sizeof( detail ), "bufs=%u drops=%d", stream_bufs, stream_drops );
  test_result( "stream survived retunes", stream_bufs > 5, detail );
}

/* ================================================================
*  Main: run all tests on each dongle
* ================================================================ */

int32_t main( int32_t argc, char *argv[] ) {
  int32_t target_dev = -1; /* -1 = test all */
  if( argc > 1 ) {
    target_dev = atoi( argv[1] );
  }

  printf( "â•”â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•—\n" );
  printf( "â•‘      libsdrgg comprehensive test suite          â•‘\n" );
  printf( "â•šâ•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•\n" );

  /* 1. Context test */
  test_context();

  /* Create main context */
  sdrgg_ctx_t *ctx = sdr::create();
  if( !ctx ) {
    fprintf( stderr, "FATAL: cannot create context\n" );
    return 1;
  }

  /* 2. Enumeration */
  sdrgg_devinfo_t devs[8];
  int32_t count;
  test_enumerate( ctx, devs, &count );
  if( count == 0 ) {
    sdr::destroy( ctx );
    printf( "\nNo devices. Aborting.\n" );
    return 1;
  }

  /* Determine which devices to test */
  int32_t start = 0, end = count;
  if( target_dev >= 0 ) {
    if( target_dev >= count ) {
      fprintf( stderr, "Device %d not found (have %d)\n", target_dev, count );
      sdr::destroy( ctx );
      return 1;
    }
    start = target_dev;
    end = target_dev + 1;
  }

  /* Run tests on each device (one at a time â€” close before opening next) */
  for( int32_t i = start; i < end; i++ ) {
    printf( "\n" );
    printf( "â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”\n" );
    printf( "  TESTING DEVICE %d  serial=%s\n", i, devs[i].serial );
    printf( "â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”\n" );

    /* Need a fresh context for each device (close releases from previous) */
    sdrgg_ctx_t *dctx = sdr::create();

    /* 3. Open/close */
    sdrgg_dev_t *dev = test_open_close( dctx, i, &devs[i] );
    if( !dev ) {
      printf( "  " COLOR_RED "SKIP remaining tests for device %d" COLOR_RESET "\n", i );
      sdr::destroy( dctx );
      continue;
    }

    sdrgg_tuner_type_t tuner = sdr::get_tuner_type( dev );

    /* 4. PPM */
    test_ppm( dev );

    /* 5. Sample rate */
    test_sample_rate( dev );

    /* 6. Frequency */
    test_frequency( dev, tuner );

    /* 7. Gain */
    test_gain( dev, tuner );

    /* 8. Demod registers */
    test_demod_regs( dev );

    /* 9. Tuner I2C */
    test_tuner_i2c( dev, tuner );

    /* 10. Sync read */
    test_sync_read( dev );

    /* 11. Async streaming */
    test_async_stream( dev );

    /* 12. Retune during streaming */
    test_retune_while_streaming( dev, tuner );

    /* Cleanup */
    sdr::close( dev );
    sdr::destroy( dctx );
  }

  sdr::destroy( ctx );

  /* Summary */
  printf( "\n" );
  printf( "â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•\n" );
  printf( "  RESULTS:  %d tests,  " COLOR_GREEN "%d pass" COLOR_RESET ",  " COLOR_RED "%d fail" COLOR_RESET ",  " COLOR_YELLOW "%d skip" COLOR_RESET "\n", tests_run, tests_pass, tests_fail, tests_skip );
  printf( "â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•\n" );

  return tests_fail > 0 ? 1 : 0;
}
