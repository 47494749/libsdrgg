# libsdrgg

Minimal zero-copy SDR hardware access layer for Linux, focused on RTL2832U-based USB receivers and low-level tuner control.

Created by Luigi Origa.

## Status

`libsdrgg` is a Linux-native C/C++ library that exposes:

- direct USB control transfers to the RTL2832U demodulator
- direct tuner I2C register access
- low-level tuning and gain control for supported tuners
- asynchronous zero-copy bulk streaming using `usbdevfs` URBs
- multi-device coordination through a single epoll-based event loop
- capability introspection for implemented and planned tuner families

The project is intentionally low-level. It does not try to hide hardware details behind a heavy abstraction layer.

## Creator And Project Intent

- Creator: Luigi Origa
- Language: C++20 with a C-compatible entry surface where appropriate
- Platform target: Linux
- Hardware family: RTL2832U / RTL2838 USB SDR dongles with external tuner ICs
- License: MIT

Project goals:

- keep the transport and tuner path explicit
- support direct register work for experiments and reverse engineering
- enable deterministic, high-throughput IQ streaming
- keep the codebase small enough to audit and extend

## Main Features

- zero-copy-style streaming pipeline based on page-aligned buffers and asynchronous USB URBs
- direct demod register access through `demod::*`
- direct tuner I2C access through `tuner::*`
- device/session lifecycle in `sdr::*`
- multi-level device reset API through `reset::*` (demod, tuner, USB, power cycle)
- chip-specific tuner code for R820T/R820T2 and FC0012
- tuner capability database already prepared for FC0013, FC2580, and E4000
- pre-stream register diagnostics (`sdrgg-regdiag`) for runtime troubleshooting
- static and shared builds from the included `Makefile`
- dedicated example programs for enumeration, capabilities, register access, sync capture, gain control, IQ file saving, chip-aware configuration, streaming, device reset, and multi-device reliability testing

## Current Tuner Support

Implemented codecs:

- R820T
- R820T2 via shared R820T codec path
- FC0012

Prepared for future support through capability descriptors only:

- FC0013
- FC2580
- E4000

## Architecture Overview

Runtime data path:

`USB transport -> RTL2832U demod/baseband -> tuner I2C control -> tuning policy -> IQ bulk stream`

Internal layer split:

- `sdrgg_usb.cpp`: raw Linux USB transport, URB allocation, event loop integration
- `sdrgg_rtl.cpp`: RTL2832U baseband and demodulator programming
- `sdrgg_core.cpp`: device lifecycle, orchestration, policy dispatch, generic API
- `sdrgg_r820t.cpp`: R820T/R820T2 tuner logic
- `sdrgg_fc0012.cpp`: FC0012 tuner logic
- `sdrgg_reset.cpp`: multi-level device reset operations (demod, tuner, USB, power)
- `sdrgg_tuner_caps.cpp`: static capability database for implemented and planned tuners
- `sdrgg.h`: public API surface
- `sdrgg_internal.h`: private state model and transport constants
- `sdrgg_r820t_internal.h`: R820T-specific internal structures and register definitions
- `sdrgg_fc0012_internal.h`: FC0012-specific internal structures and register definitions

Important design choices:

- Linux `usbdevfs` rather than libusb
- one event loop for multiple dongles
- monotonic timestamps for completed buffers
- direct register-level access available to user code

## Repository Layout

- `sdrgg.h`: public API
- `sdrgg_internal.h`: internal structs and constants
- `sdrgg_usb.cpp`: USB control/bulk transport
- `sdrgg_rtl.cpp`: RTL2832U setup and baseband programming
- `sdrgg_core.cpp`: open/close/configure/stream orchestration
- `sdrgg_r820t.cpp`: R820T/R820T2 tuning, gain and bandwidth logic
- `sdrgg_fc0012.cpp`: FC0012 tuning and gain logic
- `sdrgg_reset.cpp`: multi-level device reset (demod soft reset, tuner shadow writeback, USB reset, power cycle)
- `sdrgg_tuner_caps.cpp`: capability descriptors for all tuner families modeled so far
- `examples/enumerate_devices.cpp`: minimal device enumeration example
- `examples/show_capabilities.cpp`: capability introspection example
- `examples/register_access.cpp`: raw tuner register and R820T helper example
- `examples/read_sync_capture.cpp`: synchronous read example using `sdr::read_sync()`
- `examples/gain_control.cpp`: chip-aware gain control example for R820T/R820T2 and FC0012
- `examples/save_iq_u8.cpp`: asynchronous IQ capture saved to a raw `.u8` file
- `examples/chip_aware_device.cpp`: device configuration driven by `get_tuner_caps()` on a real tuner
- `examples/stream_capture.cpp`: minimal streaming example
- `examples/device_reset.cpp`: multi-level reset demonstration with escalation logic
- `examples/multi_device_reliability.cpp`: multi-device parallel streaming stress test
- `examples/README.md`: guide to all example programs in this directory
- `docs/r820t_notes.txt`: R820T/R820T2 notes
- `docs/fc0012_notes.txt`: FC0012 notes
- `docs/e4000_notes.txt`: E4000 notes
- `docs/rtl2832u_notes.txt`: RTL2832U notes

## Build Requirements

- Linux
- `g++` with C++20 support
- POSIX threads
- Linux USB userspace API headers
- math library

Typical packages on Debian or Ubuntu:

```bash
sudo apt update
sudo apt install build-essential linux-libc-dev
```

## Build

Static library:

```bash
make
```

Shared library:

```bash
make shared
```

Examples:

```bash
make examples
```

Install under `/usr/local` by default:

```bash
sudo make install
```

Cross-compile example for Raspberry Pi:

```bash
make CC=aarch64-linux-gnu-gcc
```

## Build Outputs

- `libsdrgg.a`
- `libsdrgg.so`
- `examples/enumerate_devices`
- `examples/show_capabilities`
- `examples/register_access`
- `examples/read_sync_capture`
- `examples/gain_control`
- `examples/save_iq_u8`
- `examples/chip_aware_device`
- `examples/stream_capture`
- `examples/multi_device_reliability`

## Permissions And Runtime Notes

The library accesses USB devices directly. On many systems you need one of these:

- run the test utilities with `sudo`
- create a udev rule granting access to RTL2832U devices

Typical IDs seen by the library:

- `0x0bda:0x2832`
- `0x0bda:0x2838`

## Public API

The public API is declared in `sdrgg.h` and organized by namespace.

### C Entry Surface

`int32_t sdrgg_enumerate( sdrgg_ctx_t *ctx, sdrgg_devinfo_t *devs, int32_t max_devs );`

Enumerates RTL2832U-class devices and fills `sdrgg_devinfo_t` entries with:

- device path
- USB VID/PID
- serial string if available
- bus and device address
- detected tuner type

### `namespace demod`

Low-level RTL2832U demodulator register I/O.

- `write(dev, block, reg, val)`
- `read(dev, block, reg, &val)`
- `write_bulk(dev, block, reg, data, len)`
- `read_bulk(dev, block, reg, data, len)`

Use these when you need direct access to RTL2832U blocks such as demod, USB, or system registers.

### `namespace tuner`

Raw tuner-side I2C access.

- `write(dev, data, len)`
- `read(dev, reg, data, len)`
- `write_reg(dev, reg, val)`
- `read_reg(dev, reg, &val)`
- `rmw(dev, reg, val, mask)`
- `write_batch(dev, reg_val_pairs, pair_count)`
- `read_batch(dev, regs, vals, count)`

This layer is the escape hatch for experiments, diagnostics, and future tuner bring-up.

### `namespace sdr`

High-level device/session API.

Lifecycle:

- `create()`
- `destroy(ctx)`
- `open(ctx, index)`
- `open_path(ctx, path)`
- `close(dev)`

RF and sampling configuration:

- `set_frequency(dev, freq_hz, &actual_hz)`
- `set_sample_rate(dev, rate_hz, &actual_hz)`
- `set_gain(dev, gain_tenth_db)`
- `get_gain(dev, &gain_tenth_db)`
- `set_freq_correction(dev, ppm)`
- `set_digital_agc(dev, enable)`

Streaming:

- `start_stream(dev, &cfg, callback, user_ctx)`
- `stop_stream(dev)`
- `read_sync(dev, buf, max_bytes, timeout_ms)`

Introspection:

- `get_tuner_type(dev)`
- `get_devinfo(dev, &info)`
- `get_xtal_freq(dev)`
- `get_tuner_caps(dev, &caps)`
- `get_caps_by_type(type)`

### `namespace r820t`

R820T and R820T2 specific helpers.

- `init(dev)`
- `standby(dev)`
- `set_freq(dev, freq_hz, if_freq_hz)`
- `pll_locked(dev, &locked)`
- `set_lna_gain(dev, index)`
- `set_mixer_gain(dev, index)`
- `set_vga_gain(dev, index)`
- `set_bandwidth(dev, bw_khz)`
- `read_signal(dev, &lna_idx, &mixer_idx)`
- `select_gain_profile(target_tenth_db)`
- `get_caps()`

Exported tables:

- `lna_db[16]`
- `mixer_db[16]`

### `namespace fc0012`

FC0012 specific helpers.

- `detect(dev)`
- `init(dev)`
- `set_freq(dev, freq_hz)`
- `set_gain(dev, gain_tenth_db)`
- `get_gains(&gains, &count)`
- `get_caps()`

### Planned Capability Namespaces

The following namespaces currently expose capability descriptors only:

- `fc0013::get_caps()`
- `fc2580::get_caps()`
- `e4000::get_caps()`

These functions are useful for user-space feature discovery even before a full codec exists.

## Capability Introspection Model

The `tuner_caps` structure exposes the properties that differ by tuner family.

Fields include:

- tuner type and chip name
- I2C address and writable register count
- frequency range and coverage gaps
- mixer architecture and IF frequency convention
- gain-stage topology
- total gain envelope
- AGC and separate-LNA-control support flags
- available bandwidth options and default bandwidth
- PLL step and fractional/integer capability flag
- crystal frequency expectation
- implementation status in the current build

This makes it possible for user space to:

- query device limits before setting values
- present chip-aware UI controls
- reject unsupported ranges early
- prepare for future tuners without branching on ad-hoc constants

## Streaming Model

Streaming uses asynchronous USB bulk URBs.

- buffers are configured with `sdrgg_stream_cfg_t`
- each completed block is returned through `sdrgg_stream_cb_t`
- each buffer carries `timestamp_us` and `sequence`
- sample format is unsigned 8-bit interleaved IQ: `I,Q,I,Q,...`

Callback contract:

- keep callbacks fast
- move or process data immediately
- avoid blocking for long periods

## Error Codes

- `SDRGG_OK`
- `SDRGG_ERR_USB`
- `SDRGG_ERR_IO`
- `SDRGG_ERR_NODEV`
- `SDRGG_ERR_BUSY`
- `SDRGG_ERR_TIMEOUT`
- `SDRGG_ERR_PLL`
- `SDRGG_ERR_PARAM`
- `SDRGG_ERR_NOMEM`

## Example

```cpp
#include <stdio.h>
#include "sdrgg.h"

int main() {
  sdrgg_ctx_t *ctx = sdr::create();
  if( !ctx ) {
    return 1;
  }

  sdrgg_devinfo_t devs[8];
  int32_t count = sdrgg_enumerate( ctx, devs, 8 );
  if( count <= 0 ) {
    sdr::destroy( ctx );
    return 1;
  }

  sdrgg_dev_t *dev = sdr::open( ctx, 0 );
  if( !dev ) {
    sdr::destroy( ctx );
    return 1;
  }

  const tuner_caps *caps = nullptr;
  if( sdr::get_tuner_caps( dev, &caps ) == SDRGG_OK ) {
    printf( "chip=%s range=%u..%u implemented=%d\n",
      caps->chip_name,
      caps->freq_min_hz,
      caps->freq_max_hz,
      caps->implemented ? 1 : 0 );
  }

  sdr::set_sample_rate( dev, 2048000, nullptr );
  sdr::set_frequency( dev, 1090000000, nullptr );
  sdr::set_gain( dev, 350 );

  sdr::close( dev );
  sdr::destroy( ctx );
  return 0;
}
```

For runnable examples, see:

- `examples/enumerate_devices.cpp`
- `examples/show_capabilities.cpp`
- `examples/register_access.cpp`
- `examples/read_sync_capture.cpp`
- `examples/gain_control.cpp`
- `examples/save_iq_u8.cpp`
- `examples/chip_aware_device.cpp`
- `examples/stream_capture.cpp`
- `examples/README.md`

## Example Programs

The repository now includes small, focused example programs under `examples/`.

Build them with:

```bash
make examples
```

Available examples:

- `examples/enumerate_devices`: list connected RTL2832U-class devices and detected tuner types
- `examples/show_capabilities`: print `tuner_caps` for all modeled tuner families, including future ones
- `examples/register_access`: read raw tuner registers and exercise R820T-specific helper APIs when applicable
- `examples/read_sync_capture`: configure one device and fetch a few blocks using `sdr::read_sync()`
- `examples/gain_control`: demonstrate chip-specific gain control for R820T/R820T2 and FC0012
- `examples/save_iq_u8`: save a short IQ capture to a raw unsigned 8-bit interleaved file
- `examples/chip_aware_device`: query real-device `tuner_caps` and configure the tuner accordingly
- `examples/stream_capture`: configure one device and capture IQ buffers for a short interval
- `examples/multi_device_reliability`: open all detected dongles in one context, stream on all of them at once, and fail if any device stalls or drops callback sequence continuity

Typical usage:

```bash
sudo ./examples/enumerate_devices
sudo ./examples/show_capabilities
sudo ./examples/register_access
sudo ./examples/read_sync_capture
sudo ./examples/gain_control
sudo ./examples/save_iq_u8
sudo ./examples/save_iq_u8 capture.u8 868.3 5
sudo ./examples/chip_aware_device
sudo ./examples/stream_capture
sudo ./examples/multi_device_reliability
```

## Supported Frequency/Gain Summary

At the time of writing:

- R820T/R820T2: 24 MHz to 1766 MHz, low-IF architecture, separate gain stages, configurable bandwidth
- FC0012: 22 MHz to 948.6 MHz, low-IF architecture, simpler gain model, no explicit public bandwidth API
- FC0013: modeled, not implemented
- FC2580: modeled, not implemented
- E4000: modeled, not implemented

## Testing

For a concurrent multi-dongle stress check, use:

```bash
sudo ./examples/multi_device_reliability
```

If another application already owns the SDRs, stop it first. For example:

```bash
sudo systemctl stop dump1090-gg
```

## Implementation Notes

- `R820T/R820T2` logic uses a three-stage model for RF path, PLL synthesis, and analog profile lowering
- `FC0012` logic uses a tuning-session pipeline for multiplier derivation, PLL packing, staging, and calibration
- capability descriptors are intentionally separate from codec implementation so user space can inspect planned tuner families before codec support lands

### R820T Gain Architecture (v1.2.0)

The gain allocation strategy distributes the requested total gain across three stages:

1. **LNA** — assigned first, stepped through the cumulative gain table until the target is reached or exceeded
2. **Mixer** — receives remaining gain, selecting the *highest* index whose cumulative gain fits within the remainder
3. **VGA** — fixed at maximum (step 15) to guarantee full 8-bit ADC dynamic range. The R820T's internal VGA AGC is enabled (register 0x0C bit 4 = 0) to let the silicon auto-adjust IF amplitude.

This design ensures optimal sensitivity at 1090 MHz and other weak-signal applications.

### Pre-stream Diagnostics

When `start_stream()` is called, the library prints a `sdrgg-regdiag` block to stderr showing key demod and tuner register values. This includes NCO frequency, resampler ratio, gain register state, and IF offset — useful for verifying the hardware path is correctly configured before data flows.

### RTL2832U IF Handling

The RTL2832U DDC soft reset clears the NCO phase accumulator. Since R820T uses a 3.57 MHz low-IF architecture, the library re-programs the IF NCO after every DDC reset to maintain proper baseband downconversion.

## Open Source Licensing

This project is distributed under the MIT License.

Local files:

- `LICENSE`
- source headers that carry `License: MIT`

License references:

- MIT license at the Open Source Initiative: <https://opensource.org/license/mit>
- SPDX identifier: <https://spdx.org/licenses/MIT.html>

## Third-Party And Platform References

`libsdrgg` is designed to work with Linux kernel userspace interfaces and public hardware knowledge. Useful references:

- Linux USB userspace API header: <https://elixir.bootlin.com/linux/latest/source/include/uapi/linux/usbdevice_fs.h>
- `epoll(7)`: <https://man7.org/linux/man-pages/man7/epoll.7.html>
- `clock_gettime(2)` and monotonic clocks: <https://man7.org/linux/man-pages/man2/clock_gettime.2.html>
- Osmocom RTL-SDR overview: <https://osmocom.org/projects/rtl-sdr/wiki>
- Linux media tree tuner drivers and public driver knowledge: <https://www.kernel.org/>

Project-local technical notes:

- `docs/rtl2832u_notes.txt`
- `docs/r820t_notes.txt`
- `docs/fc0012_notes.txt`
- `docs/e4000_notes.txt`

## Legal And Attribution Notes

- `libsdrgg` itself is MIT-licensed.
- Hardware names such as Realtek, Rafael Micro, Fitipower, and Elonics belong to their respective owners.
- Public chip knowledge in this repository is compiled from public drivers, published interface headers, and community reverse-engineering notes.
- This repository does not claim endorsement by the chip vendors.

## Roadmap Direction

- split capability descriptors for R820T and R820T2 if their published external identity needs to differ
- refine capability semantics for IF conventions and gain envelopes
- implement FC0013 codec path
- implement FC2580 codec path
- implement E4000 codec path
- add more usage examples and udev rule documentation

## Contact And Attribution

If you publish or reuse this code, keep attribution to Luigi Origa and preserve the MIT license text.
