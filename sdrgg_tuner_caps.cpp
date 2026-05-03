/*
* sdrgg_tuner_caps.cpp - Static tuner capability database
*
* Contains hardware-verified parameters for all supported tuner families.
* Each chip's capabilities are described as static const data.
* Only R820T/R820T2 and FC0012 have full codec implementations;
* the others are described here for introspection and future use.
*
* License: MIT
*/

#include "sdrgg.h"

/* ======================================================================
*  FC0013 — Fitipower (extended range sibling of FC0012)
*
*  I2C addr: 0xC6 (same as FC0012)
*  Registers: ~21 writable
*  Architecture: low-IF, single gain path with 5 states
*  BW: fixed ~6 MHz
*  PLL: integer-N with fractional extension, xtal/2 reference
*  Freq: 22 MHz – 1100 MHz (contiguous)
* ====================================================================== */

namespace fc0013 {

static const gain_stage_info fc0013_gain_stages[] = {
  { "LNA",   -99, 197, 6, nullptr },
};

static const tuner_caps fc0013_caps = {
  .tuner_type              = SDRGG_TUNER_FC0013,
  .chip_name               = "FC0013",
  .i2c_addr                = 0xC6,
  .i2c_reg_count           = 21,
  .freq_min_hz             = 22000000,
  .freq_max_hz             = 1100000000,
  .num_freq_gaps           = 0,
  .freq_gaps               = nullptr,
  .mixer_arch              = SDRGG_MIXER_LOW_IF,
  .if_freq_hz              = 0,
  .num_gain_stages         = 1,
  .gain_stages             = fc0013_gain_stages,
  .total_gain_min_tenth_db = -99,
  .total_gain_max_tenth_db = 197,
  .has_separate_lna_control = false,
  .has_agc                 = true,
  .num_bw_options          = 0,
  .bw_options              = nullptr,
  .default_bw_khz          = 6000,
  .pll_step_hz             = 1,
  .has_fractional_pll      = true,
  .xtal_freq_hz            = 28800000,
  .implemented             = false,
};

const tuner_caps *get_caps( void ) {
  return &fc0013_caps;
}

} /* namespace fc0013 */

/* ======================================================================
*  FC2580 — FCI (dual-band with coverage gap)
*
*  I2C addr: 0xAC
*  Registers: ~46 writable
*  Architecture: low-IF, separate LNA + mixer + IF VGA
*  BW: configurable 1.5/6/7/8 MHz
*  PLL: fractional-N
*  Freq: 146–308 MHz (VHF) + 438–924 MHz (UHF)
*        Gap: 308–438 MHz
* ====================================================================== */

namespace fc2580 {

static const gain_stage_info fc2580_gain_stages[] = {
  { "LNA",    0, 195, 5, nullptr },
  { "Mixer",  0, 120, 4, nullptr },
  { "IF-VGA", 0, 240, 16, nullptr },
};

static const freq_gap fc2580_gaps[] = {
  { 308000000, 438000000 },
};

static const bw_option fc2580_bw_options[] = {
  { 1530 }, { 6000 }, { 7000 }, { 8000 },
};

static const tuner_caps fc2580_caps = {
  .tuner_type              = SDRGG_TUNER_FC2580,
  .chip_name               = "FC2580",
  .i2c_addr                = 0xAC,
  .i2c_reg_count           = 46,
  .freq_min_hz             = 146000000,
  .freq_max_hz             = 924000000,
  .num_freq_gaps           = 1,
  .freq_gaps               = fc2580_gaps,
  .mixer_arch              = SDRGG_MIXER_LOW_IF,
  .if_freq_hz              = 0,
  .num_gain_stages         = 3,
  .gain_stages             = fc2580_gain_stages,
  .total_gain_min_tenth_db = 0,
  .total_gain_max_tenth_db = 555,
  .has_separate_lna_control = true,
  .has_agc                 = true,
  .num_bw_options          = 4,
  .bw_options              = fc2580_bw_options,
  .default_bw_khz          = 6000,
  .pll_step_hz             = 1,
  .has_fractional_pll      = true,
  .xtal_freq_hz            = 28800000,
  .implemented             = false,
};

const tuner_caps *get_caps( void ) {
  return &fc2580_caps;
}

} /* namespace fc2580 */

/* ======================================================================
*  E4000 — Elonics (wideband, zero-IF, quadrature mixer)
*
*  I2C addr: 0xC8
*  Registers: ~80 writable
*  Architecture: zero-IF (direct conversion), I/Q quadrature mixer
*  BW: configurable 2.4–28 MHz (analog), steps at ~5 MHz increments
*  PLL: fractional-N (sigma-delta modulator, 3rd order)
*  Freq: 52–2200 MHz with gap at ~1100–1250 MHz
*  Note: Elonics defunct; E4000 silicon is rare/collectible
* ====================================================================== */

namespace e4000 {

static const gain_stage_info e4000_gain_stages[] = {
  { "LNA",       -50, 300, 14, nullptr },
  { "Mixer",       0, 120,  2, nullptr },
  { "IF-stage1",   0,  90,  2, nullptr },
  { "IF-stage2",   0,  90,  2, nullptr },
  { "IF-stage3",   0,  90,  2, nullptr },
  { "IF-stage4",   0,  90,  2, nullptr },
  { "IF-stage5",   0,  30,  2, nullptr },
  { "IF-stage6",   0,  30,  2, nullptr },
};

static const freq_gap e4000_gaps[] = {
  { 1100000000, 1250000000 },
};

static const bw_option e4000_bw_options[] = {
  { 2400 }, { 4800 }, { 5300 }, { 7200 }, { 8700 },
  { 10000 }, { 12400 }, { 14000 }, { 17200 }, { 20000 },
  { 22400 }, { 28000 },
};

static const tuner_caps e4000_caps = {
  .tuner_type              = SDRGG_TUNER_E4000,
  .chip_name               = "E4000",
  .i2c_addr                = 0xC8,
  .i2c_reg_count           = 80,
  .freq_min_hz             = 52000000,
  .freq_max_hz             = 2200000000,
  .num_freq_gaps           = 1,
  .freq_gaps               = e4000_gaps,
  .mixer_arch              = SDRGG_MIXER_ZERO_IF,
  .if_freq_hz              = 0,
  .num_gain_stages         = 8,
  .gain_stages             = e4000_gain_stages,
  .total_gain_min_tenth_db = -50,
  .total_gain_max_tenth_db = 840,
  .has_separate_lna_control = true,
  .has_agc                 = true,
  .num_bw_options          = 12,
  .bw_options              = e4000_bw_options,
  .default_bw_khz          = 5300,
  .pll_step_hz             = 1,
  .has_fractional_pll      = true,
  .xtal_freq_hz            = 28800000,
  .implemented             = false,
};

const tuner_caps *get_caps( void ) {
  return &e4000_caps;
}

} /* namespace e4000 */
