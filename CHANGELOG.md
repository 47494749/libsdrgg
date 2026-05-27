# libsdrgg Changelog

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
