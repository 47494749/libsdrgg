/*
* sdrgg_r820t.cpp - R820T/R820T2 tuner codec
*
* Architecture (three-tier processing):
*   Domain reasoning  - abstract RF path and analog profile decisions
*   Mathematical core - PLL candidate exploration and ranking
*   Hardware lowering - register encoding and bus emission
*
* Processing flow for tuning:
*   1. Classify RF path (family + band) from frequency
*   2. Lower path plan to register encoding
*   3. Explore PLL candidates mathematically
*   4. Rank and select best candidate
*   5. Pack chosen solution into register words
*   6. Emit via intent sequences
*   7. Apply post-lock analog profile
*
* Hardware register values derived from public community research
* and direct experimentation on physical R820T/R820T2 silicon.
*
* License: MIT
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sdrgg_internal.h"
#include "sdrgg_r820t_internal.h"

namespace r820t {

/* ======================================================================
*  Register address namespace
*  Maps symbolic names to physical R820T register file (0x05..0x1F).
* ====================================================================== */

namespace addr {
  constexpr uint8_t LNA_BIAS      = 0x05;
  constexpr uint8_t FILTER_PATH   = 0x06;
  constexpr uint8_t MIXER_DRIVE   = 0x07;
  constexpr uint8_t SUPPLY_A      = 0x08;
  constexpr uint8_t SUPPLY_B      = 0x09;
  constexpr uint8_t FILT_BIAS     = 0x0A;
  constexpr uint8_t HP_CONTROL    = 0x0B;
  constexpr uint8_t IF_STAGE      = 0x0C;
  constexpr uint8_t LNA_THRESHOLD = 0x0D;
  constexpr uint8_t MIX_THRESHOLD = 0x0E;
  constexpr uint8_t CLK_ROUTING   = 0x0F;
  constexpr uint8_t XDIV_PLL      = 0x10;
  constexpr uint8_t CHARGE_PUMP   = 0x11;
  constexpr uint8_t VCO_DRIVE     = 0x12;
  constexpr uint8_t VCO_CALIB     = 0x13;
  constexpr uint8_t PLL_WORD      = 0x14;
  constexpr uint8_t SDM_FRAC_LO   = 0x15;
  constexpr uint8_t SDM_FRAC_HI   = 0x16;
  constexpr uint8_t DRAIN_PATH    = 0x17;
  constexpr uint8_t RESERVED_18   = 0x18;
  constexpr uint8_t POLY_SELECT   = 0x19;
  constexpr uint8_t MUX_ROUTING   = 0x1A;
  constexpr uint8_t TRACK_FILTER  = 0x1B;
  constexpr uint8_t RF_TOP        = 0x1C;
  constexpr uint8_t DETECT_BW     = 0x1D;
  constexpr uint8_t LNA_POWER     = 0x1E;
  constexpr uint8_t TAIL_ATTEN    = 0x1F;
} /* namespace addr */

/* ======================================================================
*  Power-up register image composition
*
*  Built from functional domain contributors, each keyed by
*  physical register ADDRESS (not positional offset). The builder
*  merges contributions into the 27-register map by direct
*  address indexing.
*
*  Each contributor describes a functional subsystem's default
*  configuration; the merge is register-address based.
* ====================================================================== */

namespace powerup {

/* Contributor: RF front-end signal path defaults */
static const regmap_contribution frontend_defaults[] = {
  { addr::LNA_BIAS,    0x83 },   /* LNA: moderate bias, AGC threshold active */
  { addr::FILTER_PATH, 0x32 },   /* Input filter: narrow path, normal current */
  { addr::MIXER_DRIVE, 0x75 },   /* Mixer: enabled, nominal drive strength */
};

/* Contributor: Power supply and bias network */
static const regmap_contribution supply_defaults[] = {
  { addr::SUPPLY_A,    0xC0 },   /* Main analog rail enabled */
  { addr::SUPPLY_B,    0x40 },   /* Auxiliary rail standby mode */
  { addr::FILT_BIAS,   0xD6 },   /* Filter bias: elevated for selectivity */
  { addr::HP_CONTROL,  0x6C },   /* HP corner: mid-band default */
};

/* Contributor: IF processing and detection thresholds */
static const regmap_contribution if_chain_defaults[] = {
  { addr::IF_STAGE,      0xE5 },   /* VGA mid-gain, IF AGC mode */
  { addr::LNA_THRESHOLD, 0x63 },   /* LNA AGC: mid-sensitivity trigger */
  { addr::MIX_THRESHOLD, 0x75 },   /* Mixer AGC: nominal trigger */
  { addr::CLK_ROUTING,   0x68 },   /* Clock: internal routing only */
};

/* Contributor: PLL subsystem idle state */
static const regmap_contribution pll_idle_defaults[] = {
  { addr::XDIV_PLL,    0x6C },   /* Crystal divider neutral */
  { addr::CHARGE_PUMP, 0x83 },   /* Charge pump: minimum bias */
  { addr::VCO_DRIVE,   0x80 },   /* VCO: nominal idle current */
  { addr::VCO_CALIB,   0x00 },   /* Calibration: inactive */
  { addr::PLL_WORD,    0x0F },   /* PLL integer word: reset */
  { addr::SDM_FRAC_LO, 0x00 },   /* SDM: zero fractional */
  { addr::SDM_FRAC_HI, 0xC0 },   /* SDM: integer mode flag */
};

/* Contributor: Signal routing fabric */
static const regmap_contribution routing_defaults[] = {
  { addr::DRAIN_PATH,   0x30 },   /* Drain: default bias routing */
  { addr::RESERVED_18,  0x48 },   /* Silicon default */
  { addr::POLY_SELECT,  0xCC },   /* Matched to default band */
  { addr::MUX_ROUTING,  0x60 },   /* Direct path selected */
  { addr::TRACK_FILTER, 0x00 },   /* Tracking filter bypassed */
};

/* Contributor: Output stage and envelope detection */
static const regmap_contribution output_defaults[] = {
  { addr::RF_TOP,     0x54 },   /* RF top: mid-range clipping */
  { addr::DETECT_BW,  0xAE },   /* Detection BW: wide */
  { addr::LNA_POWER,  0x4A },   /* LNA output: enabled, nominal */
  { addr::TAIL_ATTEN, 0xC0 },   /* Tail attenuation: none */
};

/* Merge a contributor array into the register map */
static void merge_contributor( uint8_t *regmap, const regmap_contribution *entries, uint32_t count ) {
  for( uint32_t i = 0; i < count; i++ ) {
    uint8_t idx = entries[i].reg_addr - SDRGG_R820T_REG_START;
    if( idx < REGMAP_SIZE ) {
      regmap[idx] = entries[i].value;
    }
  }
}

} /* namespace powerup */

/* Compose the full power-on image from all functional contributors */
static void compose_powerup_image( uint8_t *dest ) {
  memset( dest, 0, REGMAP_SIZE );
  powerup::merge_contributor( dest, powerup::frontend_defaults,
    sizeof( powerup::frontend_defaults ) / sizeof( powerup::frontend_defaults[0] ) );
  powerup::merge_contributor( dest, powerup::supply_defaults,
    sizeof( powerup::supply_defaults ) / sizeof( powerup::supply_defaults[0] ) );
  powerup::merge_contributor( dest, powerup::if_chain_defaults,
    sizeof( powerup::if_chain_defaults ) / sizeof( powerup::if_chain_defaults[0] ) );
  powerup::merge_contributor( dest, powerup::pll_idle_defaults,
    sizeof( powerup::pll_idle_defaults ) / sizeof( powerup::pll_idle_defaults[0] ) );
  powerup::merge_contributor( dest, powerup::routing_defaults,
    sizeof( powerup::routing_defaults ) / sizeof( powerup::routing_defaults[0] ) );
  powerup::merge_contributor( dest, powerup::output_defaults,
    sizeof( powerup::output_defaults ) / sizeof( powerup::output_defaults[0] ) );
}

/* ======================================================================
*  Dormancy intent generator
*
*  Produces the intent sequence for entering low-power state.
*  Order constraint: filter path → RF chain → bias circuits.
* ====================================================================== */

static void compose_dormancy_intents( intent_seq *seq ) {
  iseq_clear( seq );
  iseq_push_full( seq, addr::FILTER_PATH,   0xB1 );
  iseq_push_full( seq, addr::LNA_BIAS,      0x03 );
  iseq_push_full( seq, addr::MIXER_DRIVE,   0x3A );
  iseq_push_full( seq, addr::SUPPLY_A,      0x40 );
  iseq_push_full( seq, addr::SUPPLY_B,      0xC0 );
  iseq_push_full( seq, addr::FILT_BIAS,     0x36 );
  iseq_push_full( seq, addr::IF_STAGE,      0x35 );
  iseq_push_full( seq, addr::CLK_ROUTING,   0x68 );
  iseq_push_full( seq, addr::CHARGE_PUMP,   0x03 );
  iseq_push_full( seq, addr::DRAIN_PATH,    0xF4 );
  iseq_push_full( seq, addr::POLY_SELECT,   0x0C );
}

/* ======================================================================
*  RF path classification — Two-stage architecture
*
*  Stage 1: Determine abstract routing plan (family + band ID)
*           based purely on frequency domain properties.
*
*  Stage 2: Lower the routing plan into register-level encoding.
*           This is a separate pure-data transformation.
*
*  The physical LC network properties dictate three families:
*    LOW_PASS_COUPLED  (< ~80 MHz)
*    DIRECT_THROUGH    (80 – 310 MHz)
*    HIGH_BYPASS       (> 310 MHz)
*
*  Within each family, tracking bands adjust filter coefficients
*  for the frequency-dependent LC response.
* ====================================================================== */

/* Stage 1: Classify frequency into abstract routing plan */
static rf_routing_plan classify_rf_path( uint32_t freq_hz ) {
  rf_routing_plan plan;
  uint32_t mhz = freq_hz / 1000000;

  /* Determine physical family from frequency region */
  if( mhz < 80 ) {
    plan.family = rf_path_family::LOW_PASS_COUPLED;
  } else if( mhz < 310 ) {
    plan.family = rf_path_family::DIRECT_THROUGH;
  } else {
    plan.family = rf_path_family::HIGH_BYPASS;
  }

  /* Assign tracking band within family */
  if( mhz < 50 )       { plan.band = tracking_band_id::BAND_VLF_A; }
  else if( mhz < 55 )  { plan.band = tracking_band_id::BAND_VLF_B; }
  else if( mhz < 60 )  { plan.band = tracking_band_id::BAND_VLF_C; }
  else if( mhz < 65 )  { plan.band = tracking_band_id::BAND_VLF_D; }
  else if( mhz < 70 )  { plan.band = tracking_band_id::BAND_VLF_E; }
  else if( mhz < 80 )  { plan.band = tracking_band_id::BAND_VLF_F; }
  else if( mhz < 90 )  { plan.band = tracking_band_id::BAND_HF_A; }
  else if( mhz < 100 ) { plan.band = tracking_band_id::BAND_HF_B; }
  else if( mhz < 110 ) { plan.band = tracking_band_id::BAND_HF_C; }
  else if( mhz < 120 ) { plan.band = tracking_band_id::BAND_HF_D; }
  else if( mhz < 140 ) { plan.band = tracking_band_id::BAND_VHF_A; }
  else if( mhz < 180 ) { plan.band = tracking_band_id::BAND_VHF_B; }
  else if( mhz < 220 ) { plan.band = tracking_band_id::BAND_VHF_C; }
  else if( mhz < 250 ) { plan.band = tracking_band_id::BAND_VHF_D; }
  else if( mhz < 280 ) { plan.band = tracking_band_id::BAND_VHF_E; }
  else if( mhz < 310 ) { plan.band = tracking_band_id::BAND_VHF_F; }
  else if( mhz < 450 ) { plan.band = tracking_band_id::BAND_UHF_A; }
  else if( mhz < 588 ) { plan.band = tracking_band_id::BAND_UHF_B; }
  else if( mhz < 650 ) { plan.band = tracking_band_id::BAND_UHF_C; }
  else                  { plan.band = tracking_band_id::BAND_UHF_D; }

  return plan;
}

/* Stage 2: Lower routing plan to register encoding.
*  Maps abstract band IDs to silicon-specific drain/mux/tracking bytes.
*  The values are determined by the R820T LC network characteristics. */
static path_encoding lower_routing_plan( const rf_routing_plan *plan ) {
  path_encoding enc;

  /* Tracking tap coefficient per band (hardware LC response) */
  static const uint8_t tap_by_band[] = {
    0xA0,  /* VLF_A */  0x80,  /* VLF_B */  0x60,  /* VLF_C */
    0x40,  /* VLF_D */  0x20,  /* VLF_E */  0x00,  /* VLF_F */
    0x60,  /* HF_A  */  0x40,  /* HF_B  */  0x20,  /* HF_C  */
    0x00,  /* HF_D  */  0xE0,  /* VHF_A */  0xC0,  /* VHF_B */
    0xA0,  /* VHF_C */  0x80,  /* VHF_D */  0x60,  /* VHF_E */
    0x40,  /* VHF_F */  0x00,  /* UHF_A */  0x00,  /* UHF_B */
    0x00,  /* UHF_C */  0x00,  /* UHF_D */
  };

  uint8_t band_idx = static_cast< uint8_t >( plan->band );
  enc.tracking_tap = ( band_idx < 20 ) ? tap_by_band[band_idx] : 0x00;

  /* Drain bias and mux selector depend on family + band edge */
  switch( plan->family ) {
  case rf_path_family::LOW_PASS_COUPLED:
    enc.drain_bias = 0x08;
    enc.mux_selector = 0x02;
    break;

  case rf_path_family::DIRECT_THROUGH:
    enc.drain_bias = 0x00;
    /* Upper direct band uses slightly different mux config */
    if( plan->band == tracking_band_id::BAND_VHF_F ) {
      enc.mux_selector = 0x40;
    } else {
      enc.mux_selector = 0x41;
    }
    break;

  case rf_path_family::HIGH_BYPASS:
    enc.drain_bias = 0x00;
    /* Upper UHF bands use narrower mux routing */
    if( plan->band >= tracking_band_id::BAND_UHF_C ) {
      enc.mux_selector = 0x40;
    } else {
      enc.mux_selector = 0x41;
    }
    break;
  }

  return enc;
}

/* Emit RF path intents from lowered encoding */
static void compose_path_intents( intent_seq *seq, const path_encoding *enc ) {
  iseq_clear( seq );
  iseq_push_field( seq, addr::DRAIN_PATH,   enc->drain_bias,    0x08 );
  iseq_push_field( seq, addr::MUX_ROUTING,  enc->mux_selector,  0xC3 );
  iseq_push_full(  seq, addr::TRACK_FILTER, enc->tracking_tap );
  iseq_push_field( seq, addr::XDIV_PLL,     0x00, 0x0B );
}

/* ======================================================================
*  PLL synthesis engine — Three-stage architecture
*
*  Stage A: Enumerate all viable candidates across divider space
*  Stage B: Apply spur avoidance normalization and rank candidates
*  Stage C: Pack the chosen solution into register encoding
*
*  VCO operating range: 1770..3540 MHz (hardware limit)
*  Divider chain: powers of 2 (2, 4, 8, 16, 32, 64)
*  Reference: crystal frequency with PPM correction
* ====================================================================== */

static constexpr uint32_t MAX_PLL_CANDIDATES = 6;

/* Stage A: Enumerate viable PLL candidates */
static uint32_t enumerate_candidates( const synth_request *req, pll_solution *candidates ) {
  uint32_t ref_khz = req->reference_hz / 1000;
  uint32_t target_khz = req->target_hz / 1000;

  /* Apply frequency correction to reference */
  if( req->correction_ppm != 0 ) {
    int64_t adjusted = (int64_t)ref_khz * ( 1000000LL + req->correction_ppm );
    ref_khz = (uint32_t)( adjusted / 1000000LL );
  }

  uint32_t pll_ref_khz = ref_khz;
  uint32_t count = 0;

  /* VCO operating window in kHz */
  static constexpr uint32_t VCO_FLOOR_KHZ = 1770000;
  static constexpr uint32_t VCO_CEIL_KHZ  = 3540000;

  for( uint8_t order = 1; order <= 6; order++ ) {
    uint32_t divisor = (uint32_t)1 << order;
    uint64_t vco_khz = (uint64_t)target_khz * divisor;

    if( vco_khz >= VCO_FLOOR_KHZ && vco_khz < VCO_CEIL_KHZ ) {
      pll_solution *sol = &candidates[count];
      sol->divider_order = order;
      sol->vco_hz = (uint32_t)( vco_khz * 1000ULL );
      sol->in_vco_range = true;

      /* Compute integer and fractional decomposition */
      sol->n_integer = (uint32_t)( vco_khz / ( 2 * pll_ref_khz ) );
      sol->fractional_num = (uint32_t)( vco_khz - (uint64_t)2 * pll_ref_khz * sol->n_integer );
      sol->fractional_denom = 2 * pll_ref_khz;
      sol->is_fractional = ( sol->fractional_num != 0 );

      /* Quality score: prefer lower divider orders (better phase noise) */
      sol->quality_score = 100 - (int32_t)order * 10;

      count++;
      if( count >= MAX_PLL_CANDIDATES ) break;
    }
  }

  return count;
}

/* Stage B: Apply spur normalization and adjust quality rankings */
static void normalize_spur_avoidance( pll_solution *candidates, uint32_t count, uint32_t pll_ref_khz ) {
  for( uint32_t i = 0; i < count; i++ ) {
    pll_solution *sol = &candidates[i];
    uint32_t frac = sol->fractional_num;
    uint32_t denom = sol->fractional_denom;

    /* Spur boundary zones (hardware-determined) */
    uint32_t zone_near    = pll_ref_khz / 64;
    uint32_t zone_far     = pll_ref_khz * 127 / 64;
    uint32_t zone_below   = pll_ref_khz * 127 / 128;
    uint32_t zone_above   = pll_ref_khz * 129 / 128;

    /* Quantize fractional to avoid spur-prone regions */
    if( frac < zone_near ) {
      frac = 0;
    } else if( frac > zone_far ) {
      frac = 0;
      sol->n_integer++;
    } else if( frac > zone_below && frac < pll_ref_khz ) {
      frac = zone_below;
    } else if( frac > pll_ref_khz && frac < zone_above ) {
      frac = zone_above;
    }

    sol->fractional_num = frac;
    sol->is_fractional = ( frac != 0 );

    /* Bonus score for integer-mode solutions (cleaner spectrum) */
    if( !sol->is_fractional ) {
      sol->quality_score += 20;
    }

    (void)denom;
  }
}

/* Select best candidate by quality score */
static const pll_solution *rank_and_select( const pll_solution *candidates, uint32_t count ) {
  if( count == 0 ) return nullptr;

  const pll_solution *best = &candidates[0];
  for( uint32_t i = 1; i < count; i++ ) {
    if( candidates[i].quality_score > best->quality_score ) {
      best = &candidates[i];
    }
  }
  return best;
}

/* Stage C: Pack solution into register encoding */
static pll_encoding pack_pll_solution( const pll_solution *sol, uint32_t pll_ref_khz ) {
  pll_encoding enc = {};

  /* Divider field encoding */
  enc.div_field = (uint8_t)( ( sol->divider_order - 1 ) << 5 );

  /* Decompose integer into NI and SI register fields */
  uint8_t ni = (uint8_t)( ( sol->n_integer - 13 ) / 4 );
  uint8_t si = (uint8_t)( sol->n_integer - 4 * ni - 13 );
  enc.ni_si_packed = ni | (uint8_t)( si << 6 );

  /* Fractional mode flag */
  enc.mode_field = sol->is_fractional ? 0x00 : 0x08;

  /* SDM word computation for fractional mode */
  if( sol->is_fractional ) {
    uint16_t accumulator = 0;
    uint16_t weight = 2;
    uint32_t residual = sol->fractional_num;

    while( residual > 1 ) {
      if( residual > ( 2 * pll_ref_khz / weight ) ) {
        accumulator += 0x8000 / ( weight / 2 );
        residual -= 2 * pll_ref_khz / weight;
        if( weight >= 0x8000 ) break;
      }
      weight <<= 1;
    }

    enc.sdm_hi = (uint8_t)( accumulator >> 8 );
    enc.sdm_lo = (uint8_t)( accumulator & 0xFF );
  } else {
    enc.sdm_hi = 0;
    enc.sdm_lo = 0;
  }

  return enc;
}

/* Compose synthesis intents from PLL encoding */
static void compose_synth_intents( intent_seq *seq, const pll_encoding *enc ) {
  iseq_clear( seq );
  /* Pre-lock: coarse autotune mode */
  iseq_push_field( seq, addr::MUX_ROUTING,  0x00, 0x0C );
  /* VCO bias: nominal current for initial lock */
  iseq_push_field( seq, addr::VCO_DRIVE,    0x80, 0xE0 );
  /* Divider configuration */
  iseq_push_field( seq, addr::XDIV_PLL,     enc->div_field, 0xE0 );
  /* Integer word */
  iseq_push_full(  seq, addr::PLL_WORD,     enc->ni_si_packed );
  /* Fractional mode control */
  iseq_push_field( seq, addr::VCO_DRIVE,    enc->mode_field, 0x08 );
  /* SDM word (2 bytes) */
  iseq_push_full(  seq, addr::SDM_FRAC_HI,  enc->sdm_hi );
  iseq_push_full(  seq, addr::SDM_FRAC_LO,  enc->sdm_lo );
}

/* ======================================================================
*  Post-lock analog profile system
*
*  After PLL lock, the front-end analog chain is configured
*  for SDR-optimized reception. This is expressed as an abstract
*  analog_profile, then lowered to register encoding, then emitted.
* ====================================================================== */

/* Default SDR reception profile (wideband, low distortion) */
static const analog_profile sdr_reception_profile = {
  .detector_sensitivity = 0xE5,
  .rf_ceiling_policy    = 0x24,
  .pump_drive_mode      = 0x38,
  .drain_routing_mode   = 0x30,
  .filter_current_mode  = 0x40,
  .lna_headroom         = 0x00,
  .tail_control         = 0x00,
  .lna_agc_threshold    = 0x53,
  .mixer_agc_threshold  = 0x75,
};

/* Lower an analog profile to register encoding */
static analog_encoding lower_analog_profile( const analog_profile *profile ) {
  analog_encoding enc;
  enc.detect_bw_field    = profile->detector_sensitivity;
  enc.rf_top_field       = profile->rf_ceiling_policy;
  enc.pump_field         = profile->pump_drive_mode;
  enc.drain_field        = profile->drain_routing_mode;
  enc.filt_bias_field    = profile->filter_current_mode;
  enc.lna_head_field     = profile->lna_headroom;
  enc.tail_field         = profile->tail_control;
  enc.lna_thresh_field   = profile->lna_agc_threshold;
  enc.mixer_thresh_field = profile->mixer_agc_threshold;
  return enc;
}

/* Compose post-lock intent sequence from analog encoding */
static void compose_analog_intents( intent_seq *seq, const analog_encoding *enc ) {
  iseq_clear( seq );
  iseq_push_field( seq, addr::DETECT_BW,     enc->detect_bw_field,  0xC7 );
  iseq_push_field( seq, addr::RF_TOP,        enc->rf_top_field,     0xF8 );
  iseq_push_field( seq, addr::CHARGE_PUMP,   enc->pump_field,       0x38 );
  iseq_push_field( seq, addr::DRAIN_PATH,    enc->drain_field,      0x30 );
  iseq_push_field( seq, addr::FILT_BIAS,     enc->filt_bias_field,  0x60 );
  iseq_push_field( seq, addr::LNA_BIAS,      enc->lna_head_field,   0x80 );
  iseq_push_field( seq, addr::TAIL_ATTEN,    enc->tail_field,       0x80 );
  /* AGC sensitivity thresholds */
  iseq_push_full(  seq, addr::LNA_THRESHOLD, enc->lna_thresh_field );
  iseq_push_full(  seq, addr::MIX_THRESHOLD, enc->mixer_thresh_field );
}

/* ======================================================================
*  Intent lowering pass
*
*  Converts an intent sequence into physical bus operations.
*  Full assignments become direct writes; field intents become RMW.
* ====================================================================== */

static int32_t lower_intents( sdrgg_dev_t *dev, const intent_seq *seq ) {
  for( uint32_t i = 0; i < seq->length; i++ ) {
    const reg_intent *ri = &seq->entries[i];
    int32_t rc;

    if( ri->action == intent_kind::SETTLE_DELAY ) {
      continue;  /* timing constraints handled elsewhere */
    }

    if( ri->field_mask == 0xFF || ri->action == intent_kind::ASSIGN_FULL ) {
      rc = tuner::write_reg( dev, ri->target, ri->value );
    } else {
      rc = tuner::rmw( dev, ri->target, ri->value, ri->field_mask );
    }

    if( rc != SDRGG_OK ) {
      return rc;
    }
  }
  return SDRGG_OK;
}

/* ======================================================================
*  Gain engine — Envelope-based architecture
*
*  Physical gain characteristics of the R820T silicon.
*  Each entry gives the INCREMENTAL gain (tenth-dB) per step.
*  These values are hardware-measured and cannot be changed.
*
*  The envelope system pre-computes valid stage-vector combinations
*  so the core can select by total gain without greedy iteration.
* ====================================================================== */

/* Per-step incremental gain (exported for envelope construction) */
const int32_t lna_db[16] = {
  0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13
};

const int32_t mixer_db[16] = {
  0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8
};

/* Greedy gain decomposition — equivalent to the legacy sequential
*  allocator: fill LNA first, then mixer, then VGA with the residual.
*  Returns a static gain_profile populated on each call. */

static gain_profile greedy_result;

const gain_profile *select_gain_profile( int32_t target_tenth_db ) {
  /* Compute cumulative gains per stage */
  int32_t lna_cum[16], mixer_cum[16];
  lna_cum[0] = 0;
  mixer_cum[0] = 0;
  for( int32_t i = 1; i < 16; i++ ) {
    lna_cum[i] = lna_cum[i-1] + lna_db[i];
    mixer_cum[i] = mixer_cum[i-1] + mixer_db[i];
  }

  int32_t remaining = target_tenth_db;
  int32_t lna_idx = 0, mix_idx = 0, vga_idx = 0;

  /* Fill LNA first */
  for( int32_t i = 1; i < 16; i++ ) {
    if( lna_cum[i] <= remaining ) {
      lna_idx = i;
    } else {
      break;
    }
  }
  remaining -= lna_cum[lna_idx];

  /* Then mixer */
  for( int32_t i = 1; i < 16; i++ ) {
    if( mixer_cum[i] <= remaining ) {
      mix_idx = i;
    } else {
      break;
    }
  }
  remaining -= mixer_cum[mix_idx];
  if( remaining < 0 ) remaining = 0;

  /* VGA always max for adequate ADC level (LNA+Mixer control RF gain) */
  vga_idx = 15;

  greedy_result.lna_index = (uint8_t)lna_idx;
  greedy_result.mixer_index = (uint8_t)mix_idx;
  greedy_result.vga_index = (uint8_t)vga_idx;
  greedy_result.total_tenth_db = lna_cum[lna_idx] + mixer_cum[mix_idx] + vga_idx * 35;

  return &greedy_result;
}

/* Stage descriptors for the three gain paths */
static const stage_descriptor lna_stage = {
  16, addr::LNA_BIAS, 0x10, 0x0F
};

static const stage_descriptor mixer_stage = {
  16, addr::MIXER_DRIVE, 0x10, 0x0F
};

static const stage_descriptor vga_stage = {
  16, addr::IF_STAGE, 0x00, 0x0F
};

/* ======================================================================
*  Stage assignment composer
*
*  Translates a logical gain index into a stage assignment,
*  handling both manual and auto (AGC) modes.
* ====================================================================== */

static stage_assignment compose_assignment( const stage_descriptor *desc, int32_t index ) {
  stage_assignment sa;
  sa.control_reg = desc->control_reg;
  sa.affected_mask = desc->auto_mode_bit | desc->step_field;

  if( index < 0 ) {
    /* AGC mode: disable manual override */
    sa.composed_bits = 0x00;
    sa.affected_mask = desc->auto_mode_bit;
  } else {
    if( index > 0x0F ) {
      index = 0x0F;
    }
    sa.composed_bits = desc->auto_mode_bit | (uint8_t)index;
  }
  return sa;
}

static int32_t apply_assignment( sdrgg_dev_t *dev, const stage_assignment *sa ) {
  return tuner::rmw( dev, sa->control_reg, sa->composed_bits, sa->affected_mask );
}

/* ======================================================================
*  PLL lock acquisition
* ====================================================================== */

static int32_t probe_lock_status( sdrgg_dev_t *dev, bool *locked ) {
  uint8_t readback[3];
  int32_t rc = tuner::read( dev, 0x00, readback, 3 );
  if( rc != SDRGG_OK ) {
    return rc;
  }
  *locked = ( readback[2] & 0x40 ) != 0;
  return SDRGG_OK;
}

/* Attempt lock with escalation to elevated VCO current if needed */
static int32_t acquire_lock( sdrgg_dev_t *dev ) {
  usleep( 10000 );

  bool locked = false;
  int32_t rc = probe_lock_status( dev, &locked );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  if( !locked ) {
    /* Escalate VCO drive current and retry */
    rc = tuner::rmw( dev, addr::VCO_DRIVE, 0x60, 0xE0 );
    if( rc != SDRGG_OK ) {
      return rc;
    }

    usleep( 10000 );
    rc = probe_lock_status( dev, &locked );
    if( rc != SDRGG_OK ) {
      return rc;
    }

    if( !locked ) {
      return SDRGG_ERR_PLL;
    }
  }

  /* Transition to fine-step autotune (8 kHz resolution) */
  return tuner::rmw( dev, addr::MUX_ROUTING, 0x08, 0x08 );
}

/* ======================================================================
*  Public API implementation
* ====================================================================== */

/* Detect R820T presence via I2C probe */
int32_t detect( sdrgg_dev_t *dev ) {
  uint8_t probe_data[5];
  int32_t rc = tuner::read( dev, 0x00, probe_data, 5 );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  if( probe_data[0] == 0x00 && probe_data[1] == 0x00 && probe_data[2] == 0x00 ) {
    return SDRGG_ERR_NODEV;
  }

  dev->identity.tuner_class = SDRGG_TUNER_R820T;
  return SDRGG_OK;
}

/* Cold-start: compose power-up image and flush to hardware */
int32_t init( sdrgg_dev_t *dev ) {
  compose_powerup_image( dev->shadow.r820t_file );

  /* Flush complete register file */
  for( int32_t i = 0; i < SDRGG_R820T_NUM_REGS; i++ ) {
    int32_t rc = tuner::write_reg( dev, SDRGG_R820T_REG_START + (uint8_t)i, dev->shadow.r820t_file[i] );
    if( rc != SDRGG_OK ) {
      return rc;
    }
  }

  /* Default gain policy: LNA auto, mixer auto, VGA mid-range */
  stage_assignment assignments[3];
  assignments[0] = compose_assignment( &lna_stage, -1 );
  assignments[1] = compose_assignment( &mixer_stage, -1 );
  assignments[2] = compose_assignment( &vga_stage, 12 );

  for( int32_t i = 0; i < 3; i++ ) {
    int32_t rc = apply_assignment( dev, &assignments[i] );
    if( rc != SDRGG_OK ) {
      return rc;
    }
  }

  return SDRGG_OK;
}

/* Transition to low-power dormancy */
int32_t standby( sdrgg_dev_t *dev ) {
  intent_seq seq;
  compose_dormancy_intents( &seq );
  return lower_intents( dev, &seq );
}

/* Configure RF signal path for target frequency */
int32_t set_mux( sdrgg_dev_t *dev, uint32_t freq_hz ) {
  /* Stage 1: classify into abstract routing plan */
  rf_routing_plan plan = classify_rf_path( freq_hz );

  /* Stage 2: lower plan to register encoding */
  path_encoding enc = lower_routing_plan( &plan );

  /* Emit path intents */
  intent_seq seq;
  compose_path_intents( &seq, &enc );
  return lower_intents( dev, &seq );
}

/* Program PLL for target LO frequency */
int32_t set_pll( sdrgg_dev_t *dev, uint32_t freq_hz ) {
  /* Build synthesis request */
  synth_request req;
  req.target_hz = freq_hz;
  req.reference_hz = dev->baseband.oscillator_hz;
  req.correction_ppm = dev->baseband.freq_offset_ppm;

  /* Stage A: enumerate viable candidates */
  pll_solution candidates[MAX_PLL_CANDIDATES];
  uint32_t count = enumerate_candidates( &req, candidates );
  if( count == 0 ) {
    return SDRGG_ERR_PLL;
  }

  /* Compute reference for spur avoidance */
  uint32_t ref_khz = req.reference_hz / 1000;
  if( req.correction_ppm != 0 ) {
    int64_t adjusted = (int64_t)ref_khz * ( 1000000LL + req.correction_ppm );
    ref_khz = (uint32_t)( adjusted / 1000000LL );
  }

  /* Stage B: apply spur normalization and rank */
  normalize_spur_avoidance( candidates, count, ref_khz );
  const pll_solution *chosen = rank_and_select( candidates, count );
  if( !chosen ) {
    return SDRGG_ERR_PLL;
  }

  /* Stage C: pack into register encoding */
  pll_encoding enc = pack_pll_solution( chosen, ref_khz );

  /* Compose and lower synthesis intents */
  intent_seq seq;
  compose_synth_intents( &seq, &enc );
  int32_t rc = lower_intents( dev, &seq );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Acquire and verify lock */
  return acquire_lock( dev );
}

/* Complete retune session: path → synthesis → analog optimization */
int32_t set_freq( sdrgg_dev_t *dev, uint32_t freq_hz, uint32_t if_freq_hz ) {
  uint32_t lo_target = freq_hz + if_freq_hz;

  /* Phase 1: RF signal path routing */
  int32_t rc = set_mux( dev, lo_target );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Phase 2: LO synthesis */
  rc = set_pll( dev, lo_target );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  /* Phase 3: Post-lock analog profile application */
  analog_encoding aenc = lower_analog_profile( &sdr_reception_profile );
  intent_seq opt;
  compose_analog_intents( &opt, &aenc );
  return lower_intents( dev, &opt );
}

/* ---- Per-stage gain control ---- */

int32_t set_lna_gain( sdrgg_dev_t *dev, int32_t index ) {
  stage_assignment sa = compose_assignment( &lna_stage, index );
  return apply_assignment( dev, &sa );
}

int32_t set_mixer_gain( sdrgg_dev_t *dev, int32_t index ) {
  stage_assignment sa = compose_assignment( &mixer_stage, index );
  return apply_assignment( dev, &sa );
}

int32_t set_vga_gain( sdrgg_dev_t *dev, int32_t index ) {
  if( index < 0 ) index = 0;
  if( index > 15 ) index = 15;
  stage_assignment sa;
  sa.control_reg = vga_stage.control_reg;
  /* Bit 4 = 1 selects manual VGA mode.  The R820T VGA AGC (bit4=0)
   * does not work well for pulsed signals like ADS-B because it
   * settles to a gain level appropriate for the noise floor, dropping
   * brief signal pulses below the demodulator threshold. */
  sa.composed_bits = (uint8_t)( 0x10 | index );
  sa.affected_mask = 0x1F;
  return apply_assignment( dev, &sa );
}

/* ---- Filter bandwidth configuration ---- */

int32_t set_bandwidth( sdrgg_dev_t *dev, uint32_t bw_khz ) {
  /* Derive highpass corner from bandwidth target */
  uint8_t hp_encoding;
  if( bw_khz <= 200 ) {
    hp_encoding = 0x6B;
  } else if( bw_khz <= 300 ) {
    hp_encoding = 0x6A;
  } else if( bw_khz <= 500 ) {
    hp_encoding = 0x2A;
  } else {
    hp_encoding = 0x0B;
  }

  /* Derive quality and extension bits */
  uint8_t quality_control = ( bw_khz > 3000 ) ? 0x10 : 0x00;
  uint8_t boost_control = ( bw_khz <= 1000 ) ? 0x10 : 0x00;
  uint8_t extension_control = ( bw_khz > 7000 ) ? 0x80 : 0x00;

  /* Build filter intent sequence */
  intent_seq seq;
  iseq_clear( &seq );
  iseq_push_field( &seq, addr::HP_CONTROL,  hp_encoding,       0xEF );
  iseq_push_field( &seq, addr::FILT_BIAS,   quality_control,   0x10 );
  iseq_push_field( &seq, addr::FILTER_PATH, boost_control,     0x30 );
  iseq_push_field( &seq, addr::CLK_ROUTING, extension_control, 0x80 );

  return lower_intents( dev, &seq );
}

/* ---- Signal level readback ---- */

int32_t read_signal( sdrgg_dev_t *dev, uint8_t *lna_idx, uint8_t *mixer_idx ) {
  uint8_t readback[4];
  int32_t rc = tuner::read( dev, 0x00, readback, 4 );
  if( rc != SDRGG_OK ) {
    return rc;
  }

  if( lna_idx ) {
    *lna_idx = ( readback[3] >> 4 ) & 0x0F;
  }
  if( mixer_idx ) {
    *mixer_idx = readback[3] & 0x0F;
  }

  return SDRGG_OK;
}

/* ---- Lock status query (public API) ---- */

int32_t pll_locked( sdrgg_dev_t *dev, bool *locked ) {
  return probe_lock_status( dev, locked );
}

/* ---- Capability introspection ---- */

static const gain_stage_info r820t_gain_stages[] = {
  { "LNA",   0, 336, 16, lna_db },
  { "Mixer", 0, 151, 16, mixer_db },
  { "VGA",   0, 525, 16, nullptr },
};

static const bw_option r820t_bw_options[] = {
  { 200 }, { 300 }, { 500 }, { 1000 }, { 1500 }, { 2000 },
  { 3000 }, { 5000 }, { 7000 }, { 8000 },
};

static const tuner_caps r820t_caps = {
  .tuner_type              = SDRGG_TUNER_R820T,
  .chip_name               = "R820T",
  .i2c_addr                = 0x34,
  .i2c_reg_count           = 27,
  .freq_min_hz             = 24000000,
  .freq_max_hz             = 1766000000,
  .num_freq_gaps           = 0,
  .freq_gaps               = nullptr,
  .mixer_arch              = SDRGG_MIXER_LOW_IF,
  .if_freq_hz              = 3570000,
  .num_gain_stages         = 3,
  .gain_stages             = r820t_gain_stages,
  .total_gain_min_tenth_db = 0,
  .total_gain_max_tenth_db = 496,
  .has_separate_lna_control = true,
  .has_agc                 = true,
  .num_bw_options          = sizeof( r820t_bw_options ) / sizeof( r820t_bw_options[0] ),
  .bw_options              = r820t_bw_options,
  .default_bw_khz          = 6000,
  .pll_step_hz             = 1,
  .has_fractional_pll      = true,
  .xtal_freq_hz            = 28800000,
  .implemented             = true,
};

const tuner_caps *get_caps( void ) {
  return &r820t_caps;
}

} /* namespace r820t */
