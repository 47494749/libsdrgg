#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

#include "../sdrgg.h"

namespace {

constexpr int32_t kMaxDevices = 8;
constexpr int32_t kDefaultDurationS = 10;
constexpr uint32_t kDefaultSampleRateHz = 2400000;
constexpr uint32_t kStreamBufCount = 8;
constexpr uint32_t kStreamBufSize = 262144;

constexpr uint32_t kCandidateFreqsHz[] = {
  1090000000U,
  868300000U,
  131550000U,
  433920000U,
  162550000U,
};

volatile sig_atomic_t stop_requested = 0;

struct device_state {
  sdrgg_dev_t *dev = nullptr;
  sdrgg_devinfo_t info = {};
  const tuner_caps *caps = nullptr;
  uint32_t target_freq_hz = 0;
  uint32_t actual_freq_hz = 0;
  uint32_t actual_rate_hz = 0;
  int32_t gain_tenth_db = 0;
  bool opened = false;
  bool streaming = false;
  std::atomic<uint64_t> callbacks { 0 };
  std::atomic<uint64_t> bytes { 0 };
  std::atomic<uint64_t> seq_gaps { 0 };
  std::atomic<uint32_t> last_sequence { 0 };
  std::atomic<bool> have_sequence { false };
  std::atomic<uint64_t> last_callback_us { 0 };
};

static void sig_handler( int32_t sig ) {
  ( void )sig;
  stop_requested = 1;
}

static uint64_t monotonic_us( void ) {
  struct timespec ts = {};
  clock_gettime( CLOCK_MONOTONIC, &ts );
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static const char *tuner_name( sdrgg_tuner_type_t tuner ) {
  switch( tuner ) {
  case SDRGG_TUNER_R820T: return "R820T";
  case SDRGG_TUNER_R820T2: return "R820T2";
  case SDRGG_TUNER_FC0012: return "FC0012";
  case SDRGG_TUNER_FC0013: return "FC0013";
  case SDRGG_TUNER_FC2580: return "FC2580";
  case SDRGG_TUNER_E4000: return "E4000";
  default: return "Unknown";
  }
}

static bool frequency_supported( const tuner_caps *caps, uint32_t freq_hz ) {
  if( !caps ) {
    return true;
  }
  if( freq_hz < caps->freq_min_hz || freq_hz > caps->freq_max_hz ) {
    return false;
  }
  for( uint8_t i = 0; i < caps->num_freq_gaps; i++ ) {
    if( freq_hz >= caps->freq_gaps[i].start_hz && freq_hz <= caps->freq_gaps[i].stop_hz ) {
      return false;
    }
  }
  return true;
}

static uint32_t choose_frequency( const tuner_caps *caps, bool used_candidates[] ) {
  for( int32_t pass = 0; pass < 2; pass++ ) {
    for( uint32_t i = 0; i < sizeof( kCandidateFreqsHz ) / sizeof( kCandidateFreqsHz[0] ); i++ ) {
      if( !frequency_supported( caps, kCandidateFreqsHz[i] ) ) {
        continue;
      }
      if( pass == 0 && used_candidates[i] ) {
        continue;
      }
      used_candidates[i] = true;
      return kCandidateFreqsHz[i];
    }
  }

  if( caps && frequency_supported( caps, caps->freq_min_hz ) ) {
    return caps->freq_min_hz;
  }
  return 868300000U;
}

static int32_t choose_gain( const tuner_caps *caps ) {
  if( !caps ) {
    return 350;
  }
  int32_t span = caps->total_gain_max_tenth_db - caps->total_gain_min_tenth_db;
  if( span <= 0 ) {
    return caps->total_gain_max_tenth_db;
  }
  return caps->total_gain_min_tenth_db + ( span * 7 ) / 10;
}

static void stream_callback( sdrgg_dev_t *dev, const sdrgg_buffer_t *buf, void *ctx ) {
  ( void )dev;
  device_state *state = static_cast<device_state *>( ctx );
  if( !state || !buf ) {
    return;
  }

  uint32_t prev_sequence = state->last_sequence.exchange( buf->sequence, std::memory_order_relaxed );
  bool had_prev = state->have_sequence.exchange( true, std::memory_order_relaxed );
  if( had_prev && buf->sequence != prev_sequence + 1 ) {
    state->seq_gaps.fetch_add( 1, std::memory_order_relaxed );
  }

  state->callbacks.fetch_add( 1, std::memory_order_relaxed );
  state->bytes.fetch_add( buf->length, std::memory_order_relaxed );
  state->last_callback_us.store( buf->timestamp_us ? buf->timestamp_us : monotonic_us(), std::memory_order_relaxed );
}

static void stop_and_close_all( device_state devices[], int32_t count, sdrgg_ctx_t *ctx ) {
  for( int32_t i = 0; i < count; i++ ) {
    if( devices[i].streaming ) {
      sdr::stop_stream( devices[i].dev );
      devices[i].streaming = false;
    }
  }

  for( int32_t i = 0; i < count; i++ ) {
    if( devices[i].opened ) {
      sdr::close( devices[i].dev );
      devices[i].opened = false;
      devices[i].dev = nullptr;
    }
  }

  if( ctx ) {
    sdr::destroy( ctx );
  }
}

}  // namespace

int32_t main( int32_t argc, char *argv[] ) {
  int32_t duration_s = kDefaultDurationS;
  uint32_t sample_rate_hz = kDefaultSampleRateHz;

  if( argc > 1 ) {
    duration_s = atoi( argv[1] );
  }
  if( argc > 2 ) {
    sample_rate_hz = (uint32_t)strtoul( argv[2], nullptr, 10 );
  }
  if( duration_s <= 0 || sample_rate_hz == 0 ) {
    fprintf( stderr, "usage: sudo ./examples/multi_device_reliability [seconds] [sample_rate_hz]\n" );
    return 1;
  }

  signal( SIGINT, sig_handler );
  signal( SIGTERM, sig_handler );

  printf( "libsdrgg multi-device reliability test\n" );
  printf( "  duration: %d s\n", duration_s );
  printf( "  sample rate: %u Hz\n", sample_rate_hz );
  printf( "  stream cfg: %u buffers x %u bytes\n\n", kStreamBufCount, kStreamBufSize );

  sdrgg_ctx_t *ctx = sdr::create();
  if( !ctx ) {
    fprintf( stderr, "failed to create context\n" );
    return 1;
  }

  device_state devices[kMaxDevices];
  sdrgg_devinfo_t devs[kMaxDevices];
  int32_t count = sdrgg_enumerate( ctx, devs, kMaxDevices );
  if( count <= 0 ) {
    fprintf( stderr, "no RTL2832U-class devices found\n" );
    sdr::destroy( ctx );
    return 1;
  }

  printf( "found %d device(s):\n", count );
  for( int32_t i = 0; i < count; i++ ) {
    printf( "  [%d] %s serial=%s tuner=%s path=%s\n",
      i,
      devs[i].serial[0] ? devs[i].serial : "(no-serial)",
      devs[i].serial[0] ? devs[i].serial : "(no-serial)",
      tuner_name( devs[i].tuner ),
      devs[i].path );
    devices[i].info = devs[i];
  }
  printf( "\n" );

  bool used_candidates[sizeof( kCandidateFreqsHz ) / sizeof( kCandidateFreqsHz[0] )] = {};
  sdrgg_stream_cfg_t cfg = {
    .buf_count = kStreamBufCount,
    .buf_size = kStreamBufSize,
  };

  char failure_reason[256] = "";
  int32_t exit_code = 0;

  for( int32_t i = 0; i < count; i++ ) {
    devices[i].dev = sdr::open( ctx, i );
    if( !devices[i].dev ) {
      snprintf( failure_reason, sizeof( failure_reason ),
        "open failed for device %d (%s). Is another process still using the SDRs? Stop dump1090-gg first.",
        i,
        devices[i].info.serial[0] ? devices[i].info.serial : "unknown" );
      exit_code = 1;
      goto done;
    }
    devices[i].opened = true;

    sdr::get_tuner_caps( devices[i].dev, &devices[i].caps );
    devices[i].target_freq_hz = choose_frequency( devices[i].caps, used_candidates );
    devices[i].gain_tenth_db = choose_gain( devices[i].caps );

    sdr::set_freq_correction( devices[i].dev, 0 );

    int32_t rc = sdr::set_sample_rate( devices[i].dev, sample_rate_hz, &devices[i].actual_rate_hz );
    if( rc != SDRGG_OK ) {
      snprintf( failure_reason, sizeof( failure_reason ),
        "set_sample_rate failed for device %d (%s), rc=%d",
        i,
        devices[i].info.serial[0] ? devices[i].info.serial : "unknown",
        rc );
      exit_code = 1;
      goto done;
    }

    rc = sdr::set_frequency( devices[i].dev, devices[i].target_freq_hz, &devices[i].actual_freq_hz );
    if( rc != SDRGG_OK ) {
      snprintf( failure_reason, sizeof( failure_reason ),
        "set_frequency failed for device %d (%s), rc=%d",
        i,
        devices[i].info.serial[0] ? devices[i].info.serial : "unknown",
        rc );
      exit_code = 1;
      goto done;
    }

    rc = sdr::set_gain( devices[i].dev, devices[i].gain_tenth_db );
    if( rc != SDRGG_OK ) {
      snprintf( failure_reason, sizeof( failure_reason ),
        "set_gain failed for device %d (%s), rc=%d",
        i,
        devices[i].info.serial[0] ? devices[i].info.serial : "unknown",
        rc );
      exit_code = 1;
      goto done;
    }

    printf( "device[%d] serial=%s tuner=%s freq=%.3f MHz rate=%u gain=%d.%d dB\n",
      i,
      devices[i].info.serial[0] ? devices[i].info.serial : "(no-serial)",
      devices[i].caps ? devices[i].caps->chip_name : tuner_name( devices[i].info.tuner ),
      devices[i].actual_freq_hz / 1e6,
      devices[i].actual_rate_hz,
      devices[i].gain_tenth_db / 10,
      devices[i].gain_tenth_db < 0 ? -( devices[i].gain_tenth_db % 10 ) : devices[i].gain_tenth_db % 10 );
  }

  printf( "\nstarting concurrent streams...\n" );
  for( int32_t i = 0; i < count; i++ ) {
    int32_t rc = sdr::start_stream( devices[i].dev, &cfg, stream_callback, &devices[i] );
    if( rc != SDRGG_OK ) {
      snprintf( failure_reason, sizeof( failure_reason ),
        "start_stream failed for device %d (%s), rc=%d",
        i,
        devices[i].info.serial[0] ? devices[i].info.serial : "unknown",
        rc );
      exit_code = 1;
      goto done;
    }
    devices[i].streaming = true;
  }

  {
    uint64_t prev_callbacks[kMaxDevices] = {};
    uint64_t prev_bytes[kMaxDevices] = {};

    for( int32_t sec = 1; sec <= duration_s && !stop_requested; sec++ ) {
      sleep( 1 );
      uint64_t now_us = monotonic_us();

      printf( "\n[%d/%d]\n", sec, duration_s );
      for( int32_t i = 0; i < count; i++ ) {
        uint64_t callbacks = devices[i].callbacks.load( std::memory_order_relaxed );
        uint64_t bytes = devices[i].bytes.load( std::memory_order_relaxed );
        uint64_t seq_gaps = devices[i].seq_gaps.load( std::memory_order_relaxed );
        uint64_t last_callback_us = devices[i].last_callback_us.load( std::memory_order_relaxed );

        uint64_t delta_callbacks = callbacks - prev_callbacks[i];
        uint64_t delta_bytes = bytes - prev_bytes[i];
        prev_callbacks[i] = callbacks;
        prev_bytes[i] = bytes;

        double total_mb = (double)bytes / ( 1024.0 * 1024.0 );
        double rate_mb_s = (double)delta_bytes / ( 1024.0 * 1024.0 );
        uint64_t idle_ms = last_callback_us ? ( now_us - last_callback_us ) / 1000ULL : 0ULL;

        printf( "  dev[%d] serial=%s callbacks=%llu (+%llu) bytes=%.2f MB rate=%.2f MB/s gaps=%llu idle=%llums\n",
          i,
          devices[i].info.serial[0] ? devices[i].info.serial : "(no-serial)",
          (unsigned long long)callbacks,
          (unsigned long long)delta_callbacks,
          total_mb,
          rate_mb_s,
          (unsigned long long)seq_gaps,
          (unsigned long long)idle_ms );

        if( callbacks == 0 ) {
          snprintf( failure_reason, sizeof( failure_reason ),
            "device %d (%s) never delivered a callback",
            i,
            devices[i].info.serial[0] ? devices[i].info.serial : "unknown" );
          exit_code = 2;
          stop_requested = 1;
          break;
        }

        if( delta_callbacks == 0 ) {
          snprintf( failure_reason, sizeof( failure_reason ),
            "device %d (%s) stalled for at least one second",
            i,
            devices[i].info.serial[0] ? devices[i].info.serial : "unknown" );
          exit_code = 2;
          stop_requested = 1;
          break;
        }

        if( seq_gaps != 0 ) {
          snprintf( failure_reason, sizeof( failure_reason ),
            "device %d (%s) reported %llu callback sequence gap(s)",
            i,
            devices[i].info.serial[0] ? devices[i].info.serial : "unknown",
            (unsigned long long)seq_gaps );
          exit_code = 2;
          stop_requested = 1;
          break;
        }
      }
    }
  }

done:
  stop_and_close_all( devices, count > 0 ? count : 0, ctx );

  printf( "\nsummary:\n" );
  for( int32_t i = 0; i < count; i++ ) {
    printf( "  dev[%d] serial=%s callbacks=%llu bytes=%.2f MB gaps=%llu\n",
      i,
      devices[i].info.serial[0] ? devices[i].info.serial : "(no-serial)",
      (unsigned long long)devices[i].callbacks.load( std::memory_order_relaxed ),
      (double)devices[i].bytes.load( std::memory_order_relaxed ) / ( 1024.0 * 1024.0 ),
      (unsigned long long)devices[i].seq_gaps.load( std::memory_order_relaxed ) );
  }

  if( exit_code == 0 ) {
    printf( "\nPASS: all %d device(s) streamed concurrently for %d second(s) without stalls or sequence gaps.\n", count, duration_s );
    return 0;
  }

  fprintf( stderr, "\nFAIL: %s\n", failure_reason[0] ? failure_reason : "unknown problem" );
  return exit_code;
}