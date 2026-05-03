/*
* sdrgg_fc0012.cpp - FC0012 tuner codec
*
* Session-based tuning architecture:
*   Capability init    - functional domain contributors for cold start
*   Multiplier solver  - derives VCO multiplier from physical constraints
*   Ratio engine       - three-stage math → constraint → pack pipeline
*   Calibration policy - outcome classification with bias adaptation
*   Gain state machine - named operating states instead of threshold walk
*
* Processing flow for tuning:
*   1. Create tuning session with requested frequency
*   2. Derive multiplier from VCO range constraints
*   3. Solve PLL ratio mathematically
*   4. Normalize against hardware constraints
*   5. Pack into register fields
*   6. Stage hardware writes
*   7. Run calibration policy
*   8. Commit session outcome
*
* License: MIT
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "sdrgg_internal.h"
#include "sdrgg_fc0012_internal.h"

namespace fc0012 {

/* ======================================================================
*  Register address namespace
* ====================================================================== */

namespace loc {
  constexpr uint8_t RF_AMP       = 0x01;
  constexpr uint8_t RF_MIX       = 0x02;
  constexpr uint8_t FRAC_HI      = 0x03;
  constexpr uint8_t FRAC_LO      = 0x04;
  constexpr uint8_t OUT_DIV      = 0x05;
  constexpr uint8_t VCO_BW       = 0x06;
  constexpr uint8_t XTAL_CFG     = 0x07;
  constexpr uint8_t AGC_CEIL     = 0x08;
  constexpr uint8_t LOOP_PATH    = 0x09;
  constexpr uint8_t LO_DIAG      = 0x0A;
  constexpr uint8_t CLK_OUT      = 0x0B;
  constexpr uint8_t AGC_CTRL     = 0x0C;
  constexpr uint8_t LNA_OVERRIDE = 0x0D;
  constexpr uint8_t VCO_CAL      = 0x0E;
  constexpr uint8_t GAIN_CEIL    = 0x12;
  constexpr uint8_t LNA_GAIN     = 0x13;
  constexpr uint8_t LNA_COMP     = 0x15;
} /* namespace loc */

/* ======================================================================
*  Transport constants
* ====================================================================== */

static constexpr uint16_t WIRE_ADDR        = 0xC6;
static constexpr uint8_t  CHIP_ID_REG      = 0x00;
static constexpr uint8_t  CHIP_ID_EXPECT   = 0xA1;
static constexpr uint32_t BAND_SPLIT_HZ    = 300000000;

/* ======================================================================
*  Wire-level I2C bus access
* ====================================================================== */

static int32_t wire_write( sdrgg_dev_t *dev, uint8_t location, uint8_t content ) {
  uint8_t frame[2] = { location, content };

  int32_t rc = rtl::enable_i2c_repeater( dev, true );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  rc = usb::control_write( dev, WIRE_ADDR, 0x0610, frame, 2 );

  rtl::enable_i2c_repeater( dev, false );
  return rc;
}

static int32_t wire_read( sdrgg_dev_t *dev, uint8_t location, uint8_t *content ) {
  int32_t rc = rtl::enable_i2c_repeater( dev, true );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  rc = usb::control_write( dev, WIRE_ADDR, 0x0610, &location, 1 );
  if( rc != SDRGG_OK ) {
    rtl::enable_i2c_repeater( dev, false );
    return rc;
  }

  rc = usb::control_read( dev, WIRE_ADDR, 0x0600, content, 1 );

  rtl::enable_i2c_repeater( dev, false );
  return rc;
}

/* Dispatch a sequence of wire commands */
static int32_t dispatch_commands( sdrgg_dev_t *dev, const wire_cmd *cmds, uint32_t count ) {
  for( uint32_t i = 0; i < count; i++ ) {
    int32_t rc = wire_write( dev, cmds[i].location, cmds[i].content );
    if( rc != SDRGG_OK ) {
      return rc;
    }
  }
  return SDRGG_OK;
}

/* ======================================================================
*  Initialization — Capability block architecture
*
*  Cold start is composed from independent functional capability
*  blocks. Each block configures one subsystem domain.
*  The blocks are applied in dependency order.
* ====================================================================== */

/* Capability: RF front-end amplifier and mixer defaults */
static const wire_cmd rf_frontend_cmds[] = {
  { loc::RF_AMP,  0x05 },   /* RF amplifier: nominal bias */
  { loc::RF_MIX,  0x10 },   /* RF mixer: nominal drive */
  { loc::FRAC_HI, 0x00 },   /* PLL fractional: cleared */
  { loc::FRAC_LO, 0x00 },
};

/* Capability: Synthesizer subsystem idle state */
static const wire_cmd synthesizer_idle_cmds[] = {
  { loc::OUT_DIV,  0x0F },   /* Output divider: default */
  { loc::VCO_BW,   0x00 },   /* VCO BW: reset */
  { loc::XTAL_CFG, 0x20 },   /* Crystal: standard mode */
  { loc::LO_DIAG,  0xB8 },   /* LO diagnostics: off */
  { loc::VCO_CAL,  0x00 },   /* VCO calibration: idle */
};

/* Capability: AGC and feedback loop configuration */
static const wire_cmd agc_loop_cmds[] = {
  { loc::AGC_CEIL,     0xFF },   /* AGC ceiling: maximum */
  { loc::LOOP_PATH,    0x6E },   /* Loop-through: standard */
  { loc::AGC_CTRL,     0xFE },   /* AGC mode: default */
  { loc::LNA_OVERRIDE, 0x02 },   /* LNA: auto control */
};

/* Capability: Output routing and gain ceilings */
static const wire_cmd output_routing_cmds[] = {
  { loc::CLK_OUT,   0x82 },   /* Clock output: configured */
  { 0x0F,          0x00 },   /* Reserved block: zero */
  { 0x10,          0x00 },
  { 0x11,          0x00 },
  { loc::GAIN_CEIL, 0x1F },   /* Gain ceiling: maximum */
  { loc::LNA_GAIN,  0x00 },   /* LNA gain: minimum */
  { 0x14,          0x00 },   /* Reserved */
  { loc::LNA_COMP,  0x04 },   /* LNA compensation: enabled */
};

/* Capability block descriptors */
static const capability_block init_capabilities[] = {
  { rf_frontend_cmds,     sizeof( rf_frontend_cmds ) / sizeof( wire_cmd ) },
  { synthesizer_idle_cmds, sizeof( synthesizer_idle_cmds ) / sizeof( wire_cmd ) },
  { agc_loop_cmds,        sizeof( agc_loop_cmds ) / sizeof( wire_cmd ) },
  { output_routing_cmds,  sizeof( output_routing_cmds ) / sizeof( wire_cmd ) },
};

static constexpr uint32_t NUM_INIT_CAPABILITIES = sizeof( init_capabilities ) / sizeof( init_capabilities[0] );

static int32_t apply_init_capabilities( sdrgg_dev_t *dev ) {
  for( uint32_t i = 0; i < NUM_INIT_CAPABILITIES; i++ ) {
    int32_t rc = dispatch_commands( dev, init_capabilities[i].commands, init_capabilities[i].count );
    if( rc != SDRGG_OK ) {
      return rc;
    }
  }
  return SDRGG_OK;
}

/* ======================================================================
*  VCO multiplier derivation
*
*  Instead of scanning a pre-built family table, the multiplier
*  is derived from physical VCO range constraints. For each valid
*  multiplier in the hardware set, check if freq*mult falls within
*  the VCO operating window.
* ====================================================================== */

/* Derive output divider encoding from multiplier index */
static uint8_t derive_divider_encoding( uint8_t mult_index ) {
  /* Encoding pattern: pairs share a base, odd indices add 0x02 to bw */
  static const uint8_t div_base[] = {
    0x82, 0x82, 0x42, 0x42, 0x22, 0x22, 0x12, 0x12, 0x0A, 0x0A
  };
  return ( mult_index < NUM_MULTIPLIERS ) ? div_base[mult_index] : 0x0A;
}

/* Derive VCO bandwidth base from multiplier index */
static uint8_t derive_bandwidth_base( uint8_t mult_index ) {
  /* Odd indices in pairs use bw_base = 0x02, even use 0x00 */
  return ( mult_index & 1 ) ? 0x02 : 0x00;
}

static multiplier_selection derive_multiplier( uint32_t freq_hz ) {
  multiplier_selection sel = {};
  sel.valid = false;

  for( uint32_t i = 0; i < NUM_MULTIPLIERS; i++ ) {
    uint64_t vco = (uint64_t)freq_hz * MULTIPLIER_SET[i];

    /* Mirror the legacy FC0012 ladder: keep the highest multiplier whose
    * derived VCO remains below the hardware ceiling. This reproduces the
    * original threshold table without hard-coding every breakpoint. */
    if( vco < VCO_CEIL_HZ ) {
      sel.multiplier = MULTIPLIER_SET[i];
      sel.multiplier_index = (uint8_t)i;
      sel.vco_hz = vco;
      sel.elevated_bias = ( vco >= VCO_BIAS_THRESHOLD );
      sel.valid = true;
      return sel;
    }
  }

  /* Fallback to lowest multiplier */
  sel.multiplier = MULTIPLIER_SET[NUM_MULTIPLIERS - 1];
  sel.multiplier_index = (uint8_t)( NUM_MULTIPLIERS - 1 );
  sel.vco_hz = (uint64_t)freq_hz * sel.multiplier;
  sel.elevated_bias = true;
  sel.valid = true;
  return sel;
}

/* ======================================================================
*  PLL ratio engine — Three-stage pipeline
*
*  Stage 1: Compute raw ratio (mathematical rounding)
*  Stage 2: Apply hardware constraints (minimum A, maximum N)
*  Stage 3: Pack into register fields
* ====================================================================== */

/* Stage 1: Raw ratio computation */
static uint16_t compute_raw_ratio( uint64_t vco_hz, uint32_t half_xtal ) {
  uint16_t total = (uint16_t)( vco_hz / half_xtal );
  if( ( vco_hz - (uint64_t)total * half_xtal ) >= ( half_xtal / 2 ) ) {
    total++;
  }
  return total;
}

/* Stage 2: Hardware constraint normalization */
static pll_ratio_solution normalize_ratio( uint16_t raw_total, uint64_t vco_hz, uint32_t half_xtal ) {
  pll_ratio_solution sol = {};

  /* Decompose into quotient (N) and remainder (A) */
  uint8_t n = (uint8_t)( raw_total / 8 );
  uint8_t a = (uint8_t)( raw_total - 8 * n );

  /* Hardware constraint: minimum remainder of 2 */
  if( a < 2 ) {
    a += 8;
    n--;
  }

  /* Overflow clamping */
  if( n > 0x1F ) {
    sol.a_remainder = a + 8 * ( n - 0x1F );
    sol.n_quotient = 0x1F;
  } else {
    sol.a_remainder = a;
    sol.n_quotient = n;
  }

  sol.total_divider = raw_total;

  /* Feasibility bounds check */
  sol.feasible = ( sol.a_remainder <= 0x0F ) && ( sol.n_quotient >= 0x0B );

  /* Compute fractional word from VCO residual */
  uint64_t integer_hz = ( vco_hz / half_xtal ) * half_xtal;
  uint16_t frac_raw = (uint16_t)( ( vco_hz - integer_hz ) / 1000 );
  frac_raw = (uint16_t)( (uint32_t)( frac_raw << 15 ) / ( half_xtal / 1000 ) );
  if( frac_raw >= 0x4000 ) {
    frac_raw += 0x8000;
  }
  sol.frac_word = frac_raw;

  return sol;
}

/* Stage 3: Pack into register fields */
static packed_pll_regs pack_ratio( const pll_ratio_solution *sol ) {
  packed_pll_regs regs;
  regs.reg_remainder = sol->a_remainder;
  regs.reg_quotient = sol->n_quotient;
  regs.reg_frac_msb = (uint8_t)( sol->frac_word >> 8 );
  regs.reg_frac_lsb = (uint8_t)( sol->frac_word & 0xFF );
  return regs;
}

/* ======================================================================
*  Calibration policy engine
*
*  The calibration policy receives a raw measurement code,
*  classifies it against hardware thresholds, and decides
*  whether VCO bias correction is needed.
* ====================================================================== */

static calibration_verdict classify_calibration( uint8_t raw_code ) {
  if( raw_code > 0x3C ) {
    return calibration_verdict::OVER_CEILING;
  } else if( raw_code < 0x02 ) {
    return calibration_verdict::UNDER_FLOOR;
  }
  return calibration_verdict::LOCKED;
}

static calibration_outcome run_calibration_policy( sdrgg_dev_t *dev, uint8_t *bw_reg, bool elevated_bias ) {
  calibration_outcome outcome = {};
  outcome.bias_adjusted = false;

  /* Trigger measurement cycle */
  wire_write( dev, loc::VCO_CAL, 0x80 );
  wire_write( dev, loc::VCO_CAL, 0x00 );

  /* Read and classify result */
  uint8_t raw = 0;
  wire_read( dev, loc::VCO_CAL, &raw );
  outcome.raw_code = raw & 0x3F;
  outcome.verdict = classify_calibration( outcome.raw_code );

  if( outcome.verdict == calibration_verdict::LOCKED ) {
    return outcome;
  }

  /* Apply bias correction based on verdict */
  bool should_correct = false;
  if( elevated_bias && outcome.verdict == calibration_verdict::OVER_CEILING ) {
    *bw_reg &= ~0x08;
    should_correct = true;
  } else if( !elevated_bias && outcome.verdict == calibration_verdict::UNDER_FLOOR ) {
    *bw_reg |= 0x08;
    should_correct = true;
  }

  if( should_correct ) {
    wire_write( dev, loc::VCO_BW, *bw_reg );
    /* Re-trigger after bias adjustment */
    wire_write( dev, loc::VCO_CAL, 0x80 );
    wire_write( dev, loc::VCO_CAL, 0x00 );
    outcome.bias_adjusted = true;
  }

  return outcome;
}

/* ======================================================================
*  Gain state machine (named operating states)
* ====================================================================== */

static const gain_capability gain_states[] = {
  { gain_state_id::ATTENUATION, -99, 0x02 },
  { gain_state_id::MINIMUM,     -40, 0x00 },
  { gain_state_id::LOW,          71, 0x08 },
  { gain_state_id::MEDIUM,      179, 0x17 },
  { gain_state_id::HIGH,        192, 0x10 },
};

static constexpr uint32_t NUM_GAIN_STATES = sizeof( gain_states ) / sizeof( gain_states[0] );

/* Select gain state from requested level */
static const gain_capability *resolve_gain_state( int32_t requested_tenth_db ) {
  const gain_capability *selected = &gain_states[0];
  for( uint32_t i = 1; i < NUM_GAIN_STATES; i++ ) {
    if( requested_tenth_db >= gain_states[i].floor_tenth_db ) {
      selected = &gain_states[i];
    } else {
      break;
    }
  }
  return selected;
}

/* ======================================================================
*  Tuning session orchestrator
*
*  Manages the complete lifecycle of a frequency change:
*  plan → solve → pack → stage → calibrate → commit
* ====================================================================== */

static int32_t execute_tuning_session( sdrgg_dev_t *dev, tuning_session *session ) {
  uint32_t half_xtal = dev->baseband.oscillator_hz / 2;

  /* === PLAN phase === */
  session->plan = derive_multiplier( session->requested_freq_hz );
  if( !session->plan.valid ) {
    return SDRGG_ERR_PARAM;
  }

  /* === SOLVE phase === */
  uint16_t raw_ratio = compute_raw_ratio( session->plan.vco_hz, half_xtal );
  session->solution = normalize_ratio( raw_ratio, session->plan.vco_hz, half_xtal );
  if( !session->solution.feasible ) {
    fprintf( stderr, "[FC0012] no feasible PLL solution for %u Hz\n", session->requested_freq_hz );
    return SDRGG_ERR_PARAM;
  }

  /* === PACK phase === */
  session->packed = pack_ratio( &session->solution );
  session->div_encoding = derive_divider_encoding( session->plan.multiplier_index );
  session->bw_composed = derive_bandwidth_base( session->plan.multiplier_index );
  if( session->plan.elevated_bias ) {
    session->bw_composed |= 0x08;
  }
  session->bw_composed |= 0x20;  /* VCO current boost */
  session->bw_composed &= 0x3F;
  session->bw_composed |= 0x80;  /* 6 MHz bandwidth encoding */

  /* === STAGE phase — emit registers === */
  wire_cmd tuning_cmds[] = {
    { loc::RF_AMP,  session->packed.reg_remainder },
    { loc::RF_MIX,  session->packed.reg_quotient },
    { loc::FRAC_HI, session->packed.reg_frac_msb },
    { loc::FRAC_LO, session->packed.reg_frac_lsb },
    { loc::OUT_DIV, (uint8_t)( session->div_encoding | 0x07 ) },
    { loc::VCO_BW,  session->bw_composed },
  };

  int32_t rc = dispatch_commands( dev, tuning_cmds, sizeof( tuning_cmds ) / sizeof( tuning_cmds[0] ) );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* === CALIBRATE phase === */
  session->cal_result = run_calibration_policy( dev, &session->bw_composed, session->plan.elevated_bias );

  /* === COMMIT phase === */
  session->committed = true;
  return SDRGG_OK;
}

/* ======================================================================
*  Public API implementation
* ====================================================================== */

/* Detect FC0012 via chip identity register */
int32_t detect( sdrgg_dev_t *dev ) {
  rtl::set_gpio_output( dev, 4 );
  rtl::set_gpio_bit( dev, 4, 1 );
  rtl::set_gpio_bit( dev, 4, 0 );

  uint8_t chip_id = 0;
  int32_t rc = wire_read( dev, CHIP_ID_REG, &chip_id );
  if( rc != SDRGG_OK ) {
    return SDRGG_ERR_IO;
  }

  if( chip_id != CHIP_ID_EXPECT ) {
    return SDRGG_ERR_IO;
  }

  return SDRGG_OK;
}

/* Cold-start: apply all init capability blocks */
int32_t init( sdrgg_dev_t *dev ) {
  rtl::set_gpio_output( dev, 6 );
  return apply_init_capabilities( dev );
}

/* Tuning: create and execute a tuning session */
int32_t set_freq( sdrgg_dev_t *dev, uint32_t freq_hz ) {
  /* Band selection via GPIO */
  rtl::set_gpio_bit( dev, 6, ( freq_hz > BAND_SPLIT_HZ ) ? 1 : 0 );

  /* Create and execute tuning session */
  tuning_session session = {};
  session.requested_freq_hz = freq_hz;
  session.committed = false;

  int32_t rc = execute_tuning_session( dev, &session );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  dev->tuning.center_freq_hz = freq_hz;
  return SDRGG_OK;
}

/* Gain control via state machine selection */
int32_t set_gain( sdrgg_dev_t *dev, int32_t gain_tenth_db ) {
  uint8_t current;
  int32_t rc = wire_read( dev, loc::LNA_GAIN, &current );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Resolve to named gain state */
  const gain_capability *state = resolve_gain_state( gain_tenth_db );

  /* Preserve upper control bits, apply state encoding */
  current &= 0xE0;
  return wire_write( dev, loc::LNA_GAIN, current | state->hw_code );
}

/* Enumerate available gain levels from capability states */
int32_t get_gains( const int16_t **gains, int32_t *count ) {
  static int16_t enumerated[NUM_GAIN_STATES];
  static bool populated = false;
  if( !populated ) {
    for( uint32_t i = 0; i < NUM_GAIN_STATES; i++ ) {
      enumerated[i] = gain_states[i].floor_tenth_db;
    }
    populated = true;
  }
  *gains = enumerated;
  *count = (int32_t)NUM_GAIN_STATES;
  return SDRGG_OK;
}

/* ---- Capability introspection ---- */

static const gain_stage_info fc0012_gain_stages[] = {
  { "RF", -99, 192, 5, nullptr },
};

static const tuner_caps fc0012_caps = {
  .tuner_type              = SDRGG_TUNER_FC0012,
  .chip_name               = "FC0012",
  .i2c_addr                = 0xC6,
  .i2c_reg_count           = 22,
  .freq_min_hz             = 22000000,
  .freq_max_hz             = 948600000,
  .num_freq_gaps           = 0,
  .freq_gaps               = nullptr,
  .mixer_arch              = SDRGG_MIXER_LOW_IF,
  .if_freq_hz              = 0,
  .num_gain_stages         = 1,
  .gain_stages             = fc0012_gain_stages,
  .total_gain_min_tenth_db = -99,
  .total_gain_max_tenth_db = 192,
  .has_separate_lna_control = false,
  .has_agc                 = true,
  .num_bw_options          = 0,
  .bw_options              = nullptr,
  .default_bw_khz          = 6000,
  .pll_step_hz             = 1,
  .has_fractional_pll      = true,
  .xtal_freq_hz            = 28800000,
  .implemented             = true,
};

const tuner_caps *get_caps( void ) {
  return &fc0012_caps;
}

} /* namespace fc0012 */
