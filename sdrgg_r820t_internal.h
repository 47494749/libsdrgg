/*
* sdrgg_r820t_internal.h - R820T tuner codec private types
*
* Three-tier type architecture:
*   Abstract domain  - RF path families, tracking profiles, analog policies
*   Candidate math   - PLL solution space exploration (pure math, no registers)
*   Hardware packing - final register-level encoding for bus emission
*
* The abstract tier describes WHAT the tuner should achieve.
* The candidate tier explores HOW to achieve it mathematically.
* The packing tier translates a chosen solution into silicon words.
*
* License: MIT
*/

#ifndef SDRGG_R820T_INTERNAL_H
#define SDRGG_R820T_INTERNAL_H

#include <stdint.h>

namespace r820t {

/* ======================================================================
*  TIER 1: Abstract RF domain
*
*  These types describe tuner intent without referencing
*  any register encoding. They model the RF signal path
*  and operating policy at a physics/system level.
* ====================================================================== */

/* Physical signal routing topology */
enum class rf_path_family : uint8_t {
  LOW_PASS_COUPLED,      /* LC low-pass network (sub-80 MHz) */
  DIRECT_THROUGH,        /* Direct coupling, tracking filter active */
  HIGH_BYPASS,           /* Filter bypass for UHF+ */
};

/* Tracking filter operating band identifier */
enum class tracking_band_id : uint8_t {
  BAND_VLF_A   =  0,    /* < 50 MHz: maximum tracking */
  BAND_VLF_B   =  1,    /* 50-55 MHz */
  BAND_VLF_C   =  2,    /* 55-60 MHz */
  BAND_VLF_D   =  3,    /* 60-65 MHz */
  BAND_VLF_E   =  4,    /* 65-70 MHz */
  BAND_VLF_F   =  5,    /* 70-80 MHz */
  BAND_HF_A    =  6,    /* 80-90 MHz */
  BAND_HF_B    =  7,    /* 90-100 MHz */
  BAND_HF_C    =  8,    /* 100-110 MHz */
  BAND_HF_D    =  9,    /* 110-120 MHz */
  BAND_VHF_A   = 10,    /* 120-140 MHz */
  BAND_VHF_B   = 11,    /* 140-180 MHz */
  BAND_VHF_C   = 12,    /* 180-220 MHz */
  BAND_VHF_D   = 13,    /* 220-250 MHz */
  BAND_VHF_E   = 14,    /* 250-280 MHz */
  BAND_VHF_F   = 15,    /* 280-310 MHz */
  BAND_UHF_A   = 16,    /* 310-450 MHz */
  BAND_UHF_B   = 17,    /* 450-588 MHz */
  BAND_UHF_C   = 18,    /* 588-650 MHz */
  BAND_UHF_D   = 19,    /* 650+ MHz */
};

/* Complete RF path routing decision (abstract, no register bytes) */
struct rf_routing_plan {
  rf_path_family    family;
  tracking_band_id  band;
};

/* Analog front-end operating profile (post-lock tuning policy) */
struct analog_profile {
  uint8_t  detector_sensitivity;   /* detection bandwidth policy */
  uint8_t  rf_ceiling_policy;      /* RF top clipping control */
  uint8_t  pump_drive_mode;        /* charge pump bias policy */
  uint8_t  drain_routing_mode;     /* drain path bias */
  uint8_t  filter_current_mode;    /* filter bias current */
  uint8_t  lna_headroom;           /* LNA input headroom */
  uint8_t  tail_control;           /* output stage attenuation */
  uint8_t  lna_agc_threshold;      /* LNA AGC threshold */
  uint8_t  mixer_agc_threshold;    /* mixer AGC threshold */
};

/* ======================================================================
*  TIER 2: Candidate mathematics (PLL solution space)
*
*  Pure mathematical representation of PLL synthesis solutions.
*  No register fields, no silicon encoding.
* ====================================================================== */

/* Input specification for PLL solver */
struct synth_request {
  uint32_t target_hz;       /* desired LO frequency */
  uint32_t reference_hz;    /* crystal oscillator frequency */
  int32_t  correction_ppm;  /* frequency correction */
};

/* Single mathematical PLL solution candidate */
struct pll_solution {
  uint8_t  divider_order;     /* VCO divider as power of 2 (1..6) */
  uint32_t vco_hz;            /* resulting VCO frequency */
  uint32_t n_integer;         /* integer part of PLL ratio */
  uint32_t fractional_num;    /* fractional numerator (0 = integer mode) */
  uint32_t fractional_denom;  /* fractional denominator (2 * pll_ref) */
  bool     is_fractional;     /* true if frac-N required */
  bool     in_vco_range;      /* true if VCO within operating window */
  int32_t  quality_score;     /* higher = better (closeness metric) */
};

/* ======================================================================
*  TIER 3: Hardware packing (register-level encoding)
*
*  These types are produced ONLY after a solution is chosen.
*  They contain the final byte-level values for bus emission.
* ====================================================================== */

/* RF path register encoding (produced from rf_routing_plan) */
struct path_encoding {
  uint8_t  drain_bias;       /* drain routing control byte */
  uint8_t  mux_selector;     /* multiplexer path byte */
  uint8_t  tracking_tap;     /* tracking filter coefficient byte */
};

/* PLL register encoding (produced from pll_solution) */
struct pll_encoding {
  uint8_t  div_field;        /* VCO divider register bits */
  uint8_t  ni_si_packed;     /* combined NI|SI register word */
  uint8_t  mode_field;       /* integer/fractional selector bit */
  uint8_t  sdm_hi;           /* sigma-delta modulator high byte */
  uint8_t  sdm_lo;           /* sigma-delta modulator low byte */
};

/* Analog profile register encoding (produced from analog_profile) */
struct analog_encoding {
  uint8_t  detect_bw_field;
  uint8_t  rf_top_field;
  uint8_t  pump_field;
  uint8_t  drain_field;
  uint8_t  filt_bias_field;
  uint8_t  lna_head_field;
  uint8_t  tail_field;
  uint8_t  lna_thresh_field;
  uint8_t  mixer_thresh_field;
};

/* ======================================================================
*  Register intent primitives (bus emission abstraction)
*
*  Decouples tuning logic from transport. The engine declares
*  intents; a lowering pass converts them to bus operations.
* ====================================================================== */

enum class intent_kind : uint8_t {
  SET_FIELD,          /* write specific bits under mask */
  ASSIGN_FULL,        /* full register overwrite */
  SETTLE_DELAY,       /* timing constraint (no register) */
};

struct reg_intent {
  intent_kind  action;
  uint8_t      target;      /* register address */
  uint8_t      value;       /* desired value or bits */
  uint8_t      field_mask;  /* which bits this intent covers */
};

static constexpr uint32_t INTENT_SEQ_CAPACITY = 32;

struct intent_seq {
  reg_intent   entries[INTENT_SEQ_CAPACITY];
  uint32_t     length;
};

static inline void iseq_clear( intent_seq *s ) {
  s->length = 0;
}

static inline bool iseq_push( intent_seq *s, intent_kind action, uint8_t target, uint8_t value, uint8_t mask ) {
  if( s->length >= INTENT_SEQ_CAPACITY ) {
    return false;
  }
  s->entries[s->length] = { action, target, value, mask };
  s->length++;
  return true;
}

static inline bool iseq_push_full( intent_seq *s, uint8_t target, uint8_t value ) {
  return iseq_push( s, intent_kind::ASSIGN_FULL, target, value, 0xFF );
}

static inline bool iseq_push_field( intent_seq *s, uint8_t target, uint8_t value, uint8_t mask ) {
  return iseq_push( s, intent_kind::SET_FIELD, target, value, mask );
}

/* ======================================================================
*  Gain envelope (pre-composed stage-vector configurations)
*
*  Instead of greedy per-step allocation, the gain engine
*  works with a list of pre-validated gain profiles.
*  The gain_profile struct is declared in sdrgg.h (public API).
* ====================================================================== */

/* Stage descriptor for individual gain paths */
struct stage_descriptor {
  uint8_t  max_steps;
  uint8_t  control_reg;
  uint8_t  auto_mode_bit;
  uint8_t  step_field;
};

struct stage_assignment {
  uint8_t  control_reg;
  uint8_t  composed_bits;
  uint8_t  affected_mask;
};

/* ======================================================================
*  Power-up register map builder
*
*  The init image is composed from functional contributors
*  that write by register address (not positional offset).
*  The builder merges contributions into the final register map.
* ====================================================================== */

static constexpr uint32_t REGMAP_SIZE = 27;  /* R820T register file 0x05..0x1F */

struct regmap_contribution {
  uint8_t  reg_addr;     /* physical register address */
  uint8_t  value;        /* silicon-required value */
};

} /* namespace r820t */

#endif /* SDRGG_R820T_INTERNAL_H */
