/*
* sdrgg_fc0012_internal.h - FC0012 tuner codec private types
*
* Three-tier type architecture:
*   Domain model     - VCO multiplier rules, PLL ratio mathematics
*   Tuning session   - plan/encode/stage/validate/commit lifecycle
*   Hardware action   - typed actions lowered to wire commands
*
* The domain model expresses tuner physics (multiplier relationships,
* ratio constraints, calibration thresholds).
* The session model manages the lifecycle of a frequency change.
* Hardware actions are the final register-level operations.
*
* License: MIT
*/

#ifndef SDRGG_FC0012_INTERNAL_H
#define SDRGG_FC0012_INTERNAL_H

#include <stdint.h>

namespace fc0012 {

/* ======================================================================
*  TIER 1: Domain model (physics and mathematics)
*
*  VCO multiplier rules, not tabulated families.
*  The FC0012 VCO operates at N * freq_hz, where N is chosen
*  from a fixed set of valid multipliers based on VCO range limits.
* ====================================================================== */

/* Valid VCO multiplier set (hardware-fixed divider chain) */
static constexpr uint8_t MULTIPLIER_SET[] = {
  96, 64, 48, 32, 24, 16, 12, 8, 6, 4
};
static constexpr uint32_t NUM_MULTIPLIERS = sizeof( MULTIPLIER_SET ) / sizeof( MULTIPLIER_SET[0] );

/* VCO selection threshold (Hz)
*  FC0012 chooses the highest multiplier whose derived VCO stays below
*  the ceiling, matching the legacy threshold ladder. */
static constexpr uint64_t VCO_CEIL_HZ  = 3060000000ULL;

/* Elevated bias threshold (VCO frequency) */
static constexpr uint64_t VCO_BIAS_THRESHOLD = 0x00000000B6630B80ULL;

/* Multiplier selection result (derived, not tabulated) */
struct multiplier_selection {
  uint8_t  multiplier;         /* chosen VCO multiplier (from set above) */
  uint8_t  multiplier_index;   /* index in MULTIPLIER_SET */
  uint64_t vco_hz;             /* resulting VCO frequency */
  bool     elevated_bias;      /* needs increased VCO bias */
  bool     valid;              /* selection succeeded */
};

/* ======================================================================
*  TIER 2: PLL ratio mathematics (pure math, no registers)
*
*  The two-stage solver computes a rational approximation of
*  the VCO frequency as a quotient + remainder + fraction.
* ====================================================================== */

/* Mathematical PLL ratio solution */
struct pll_ratio_solution {
  uint16_t total_divider;    /* total integer ratio (N*8 + A) */
  uint8_t  n_quotient;       /* integer quotient (N register value) */
  uint8_t  a_remainder;      /* integer remainder (A register value) */
  uint16_t frac_word;        /* fractional word (0 = integer mode) */
  bool     feasible;         /* within hardware constraints */
};

/* Packed register fields (produced from pll_ratio_solution) */
struct packed_pll_regs {
  uint8_t  reg_remainder;    /* A register encoding */
  uint8_t  reg_quotient;     /* N register encoding */
  uint8_t  reg_frac_msb;    /* fractional high byte */
  uint8_t  reg_frac_lsb;    /* fractional low byte */
};

/* ======================================================================
*  TIER 3: Hardware actions (typed intent → wire command)
*
*  Instead of raw wire_cmd lists, tuning is expressed as
*  typed actions that a dispatcher lowers to register writes.
* ====================================================================== */

/* Action types for tuning session */
enum class tuning_action_type : uint8_t {
  SELECT_BAND,           /* GPIO band switch */
  APPLY_PLL_RATIO,       /* Write N, A, frac registers */
  SET_OUTPUT_DIVIDER,    /* Configure output divider */
  SET_VCO_BANDWIDTH,     /* VCO bias and bandwidth */
  TRIGGER_CALIBRATION,   /* Toggle calibration trigger */
  APPLY_GAIN_CODE,       /* Set gain register */
};

/* Wire-level write primitive (lowest level) */
struct wire_cmd {
  uint8_t location;     /* register address */
  uint8_t content;      /* value to write */
};

/* ======================================================================
*  Calibration policy (outcome classification)
* ====================================================================== */

enum class calibration_verdict : uint8_t {
  LOCKED,              /* calibration within acceptable range */
  OVER_CEILING,        /* VCO above ceiling (>0x3C) - reduce bias */
  UNDER_FLOOR,         /* VCO below floor (<0x02) - increase bias */
};

struct calibration_outcome {
  uint8_t              raw_code;
  calibration_verdict  verdict;
  bool                 bias_adjusted;
};

/* ======================================================================
*  Gain capability model (named operating states)
* ====================================================================== */

enum class gain_state_id : uint8_t {
  ATTENUATION    = 0,     /* maximum attenuation */
  MINIMUM        = 1,     /* minimum active gain */
  LOW            = 2,     /* low gain */
  MEDIUM         = 3,     /* medium gain */
  HIGH           = 4,     /* high/maximum gain */
};

struct gain_capability {
  gain_state_id  state;
  int16_t        floor_tenth_db;   /* minimum gain for this state */
  uint8_t        hw_code;          /* hardware encoding */
};

/* ======================================================================
*  Tuning session state (lifecycle object)
* ====================================================================== */

struct tuning_session {
  /* Request */
  uint32_t             requested_freq_hz;

  /* Plan phase */
  multiplier_selection plan;

  /* Solve phase */
  pll_ratio_solution   solution;

  /* Pack phase */
  packed_pll_regs      packed;
  uint8_t              div_encoding;
  uint8_t              bw_composed;

  /* Outcome */
  calibration_outcome  cal_result;
  bool                 committed;
};

/* ======================================================================
*  Init capability blocks (functional subsystem descriptors)
* ====================================================================== */

struct capability_block {
  const wire_cmd *commands;
  uint32_t        count;
};

} /* namespace fc0012 */

#endif /* SDRGG_FC0012_INTERNAL_H */
