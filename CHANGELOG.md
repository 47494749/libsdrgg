# libsdrgg Changelog

## v1.3.1 — 2026-09-18

### FC0012 I2C protocol fix and RTL2832U firmware-aware hardening

Root cause analysis of the FC0012 "deaf tuner" problem revealed two distinct
issues, both traced to how the RTL2832U 8051 firmware (mask ROM) handles I2C:

**Bug 1 — Wrong I2C read protocol.** `tuner::read()` used bulk sequential read
(one USB control transfer reads N bytes starting from register 0). This works
for R820T because its I2C slave auto-increments the address pointer. FC0012
requires two-phase random-access reads: write the register address first, then
read one byte. The bulk sequential read returned garbage for FC0012.

**Bug 2 — Streaming kills FC0012 RF reception.** The RTL2832U's usbdevfs async
URB streaming (sdrgg's zero-copy bulk path) creates a timing conflict in the
8051 firmware. The IE0 bulk-completion interrupt can preempt the CTF control
transfer handler between the two phases of an FC0012 I2C read, corrupting the
I2C state machine. R820T is unaffected because it uses a shadow register cache
and never reads tuner registers during runtime.

The 8051 ROM was dumped and disassembled (64KB, IDA Pro 8051/C517). Key finding:
the firmware at address 0x072A caches the IICB block index in RAM_8 and skips
I2C address re-setup for consecutive same-block transactions. Combined with the
IE0 bulk interrupt preemption, this creates a race window specific to FC0012's
two-phase read protocol.

**Status:** Both bugs fully fixed. Bug 2 root cause (found via usbmon capture):
`stop_bulk` writes EPA_CTL=0x1002 during stream teardown, which resets the USB
endpoint in a way that permanently corrupts FC0012 tuner state. Fix: skip
EPA_CTL reset for FC0012 in `stop_stream`, use DEMOD_CTL=0x20 (demod power-down)
in `deinit` instead. This matches librtlsdr's shutdown sequence exactly. The I2C
repeater gate is also kept open during streaming (P1[01]=0x18) to match librtlsdr.
FC0012 streaming via sdrgg is now fully functional.

### FC0012 fixes

- **Two-phase I2C read** (`tuner::read`, `i2c_session_read`): FC0012/FC0013
  now use write-address + read-data per register instead of bulk sequential.
- **I2C repeater timing**: 500µs delay after enabling the repeater gate for
  FC0012, allowing the RTL2832U demod hardware to propagate the gate state
  before I2C traffic begins.
- **VCO calibration delay**: 10ms settling time after VCO trigger (reg 0x0E),
  matching the Linux kernel fc0012 driver. Prevents stale voltage readback.
- **Gain latch prime**: `fc0012::init()` now sets minimum gain (-9.9 dB) during
  cold start, working around the silicon bug where the first gain write after
  power-on is silently ignored (librtlsdr PR#74).
- **GPIO4 + GPIO7 detect**: `fc0012::detect()` tries GPIO4 then GPIO7 as reset
  pin, covering both standard and RTL2838UHIDIR board variants.
- **Firmware I2C cache invalidation**: detect sends a dummy I2C transaction to
  address 0x00 before probing 0xC6, forcing the 8051's RAM_8 address cache to
  mismatch and re-send the slave address.
- **AGC support**: `fc0012::set_auto_gain()` implemented (clears reg 0x0D bit 3
  for hardware LNA auto mode). Registered as `apply_auto_gain_fn` in the family
  contract.
- **Bulk-pause on I2C**: `set_frequency` and `set_gain` stop the bulk endpoint
  before FC0012 I2C transactions and restart it after, preventing the IE0/CTF
  firmware race condition.
- **Clean shutdown**: `sdr::close()` calls `fc0012::set_auto_gain()` before
  baseband deinit. `rtl::deinit()` no longer writes DEMOD_CTL=0x20 (power-down),
  which was permanently killing the I2C bus for FC0012.

### Backend changes

- `sdr_backend.cpp` and `sdr_backend_sdrgg.cpp`: `supports_tuner_agc` now
  includes FC0012 and FC0013 alongside R820T/R820T2.

## v1.3.0 — 2026-07-06

### New: FC0012 tuner support

- Full FC0012 Zero-IF tuner integration: PLL tuning, VCO calibration, gain control.
- RTL2832U demodulator configuration for FC0012: Zero-IF mode (reg 0xB1=0x1B), dual I/Q ADC (reg 0x08=0xCD), no spectrum inversion, IF NCO=0.
- FC0012 registered in the family probe table (`prepare_baseband_fn`).
- LNA manual mode: set_gain now forces LNA to manual (reg 0x0D bit 3) before writing gain, preventing AGC from overriding programmed values.

### R820T gain rewrite

- Replace greedy gain decomposition with a fixed 29-step LNA/Mixer gain table matching librtlsdr exactly.
- VGA fixed at step 8 (manual mode) or step 11 (auto mode), matching standard rtl-sdr behaviour.
- `set_mixer_gain`: keep mixer AGC enabled (bit 4) even in manual mode for better linearity.
- `set_vga_gain`: clear bit 7 and bit 4, write low nibble directly (match rtl-sdr programming).
- `set_bandwidth`: complete rewrite with proper IF filter table, HP corner selection, and IF frequency adjustment (returns actual IF frequency for NCO programming).
- Auto-gain now sets LNA auto + mixer auto + VGA at index 11.

### USB transport

- CLEAR_HALT on endpoint stall: detect URB stall status and issue `USBDEVFS_CLEAR_HALT` ioctl before resubmitting, preventing permanent stream failure on intermittent USB errors.

### Bug fixes

- `sdrgg_set_gain`: properly propagate error codes; only update `gain_policy` on success.
- Auto-gain: return `SDRGG_ERR_PARAM` if the tuner family has no `apply_auto_gain_fn` registered (instead of silently proceeding).

### Cleanup

- Remove `patch_clear_halt.py` development utility script.
- Remove unconditional gain-readback debug fprintf from FC0012 set_gain.
- Version bumped to 1.3.0.

## v1.2.1 — 2026-05-27

### Release Cleanup

- Make diagnostic stderr output opt-in at build time via `SDRGG_ENABLE_DIAGNOSTICS=1` instead of always printing `sdrgg-regdiag` and `sdrgg-urb-diag` in normal builds.
- Update `README.md` build instructions to match the actual Makefile targets and cross-compilation variable (`CXX`, not `CC`).
- Align the top-level README example inventory with the current example set, including `device_reset` and `multi_device_reliability`.
- Expand `examples/README.md` purpose text so it matches the recovery and stress-test examples shipped in the tree.

## v1.2.0 — 2026-05-24

### New: Multi-Level Device Reset API (`reset::` namespace)

- **`reset::reset_demod(dev)`** — Level 1: RTL2832U demodulator soft reset via page 1 register 0x01 bit 2. Resets DSP pipeline (decimation filters, AGC accumulators) without disturbing the tuner or USB link. Sub-millisecond recovery.
- **`reset::reset_tuner(dev)`** — Level 2: Re-writes all tuner registers from the internal shadow register file. Fixes corrupted I2C/tuner state without USB disturbance. Supports R820T (full shadow writeback) and FC0012 (re-init).
- **`reset::reset_usb(dev)`** — Level 3: Issues `ioctl(USBDEVFS_RESET)` on the device fd. Resets USB protocol state; device re-enumerates on the same port without power-cycling. Device must be re-opened after this call.
- **`reset::reset_power(dev, usb_path)`** — Level 4: Disables and re-enables USB port power via sysfs `authorized` attribute. True cold reset equivalent to physical unplug/replug. Requires root. Device must be completely re-enumerated and re-opened.
- **`reset::reset_full(dev)`** — Convenience: executes levels 1+2+3 in sequence, stopping at the first unrecoverable failure. Does NOT include power cycle.
- New source file: `sdrgg_reset.cpp`
- New example: `examples/device_reset.cpp`

### Functional Changes

- **R820T gain allocation fix**: mixer gain now picks highest fitting index instead of first exceeding one, ensuring optimal RF gain distribution.
- **R820T VGA always at maximum**: VGA (IF amplifier) fixed at step 15 to guarantee adequate ADC amplitude, especially at 1090 MHz where signals are weak.
- **R820T VGA AGC enabled**: `set_vga_gain()` now clears register bit 4, enabling the R820T's internal VGA AGC (matching standard rtl-sdr behavior).
- **RTL2832U IF NCO re-program after DDC reset**: soft reset previously cleared the NCO phase accumulator, losing the 3.57 MHz Low-IF downconversion for R820T. Now re-programs from stored offset after every reset.
- **Pre-stream register diagnostics**: `start_stream()` now prints demod and tuner register state to stderr (`sdrgg-regdiag`) for runtime troubleshooting.

### Cleanup

- Removed in-tree test utilities (`sdrgg_test`, `sdrgg_fulltest`, `i2c_verify`); use examples as templates.
- Removed duplicate source files from root (already present in `examples/` and `docs/`).
- Cleaned Makefile: removed `test` and `fulltest` targets.
- Added `.gitignore` for build artifacts.
- Internal headers (`sdrgg_r820t_internal.h`, `sdrgg_fc0012_internal.h`) now documented in repository layout.
- Updated README to reflect current file structure and removed obsolete references.

## v1.1.0 — 2026-05-06

- Fragment synchronous bulk USB reads into 16KB chunks to work around the Linux `USBDEVFS_BULK` ioctl size limit. Handles short reads and partial I/O errors gracefully.
- Add library version macros for compile-time and runtime version checking.

## v1.0.0 — 2026-05-03

- Initial release: minimal zero-copy SDR hardware access layer for Linux.
- Direct usbdevfs access (no libusb dependency).
- RTL2832U baseband engine with demod register sequencing.
- R820T tuner codec with full PLL, LNA, mixer, and VGA control.
- FC0012 tuner codec with band-specific register maps.
- Asynchronous streaming via URB ring buffer with epoll event loop.
- Synchronous bulk read path.
- Multi-device support with per-context epoll thread.
- Tuner capability introspection API.
- CLOCK_MONOTONIC timestamps for cross-device coherence.
