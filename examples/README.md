# libsdrgg Examples

Example programs for `libsdrgg`, the low-level Linux SDR access library created by Luigi Origa.

## Copyright And License

Created by Luigi Origa.

These example sources are distributed under the same MIT license used by `libsdrgg`.

License references:

- MIT license at the Open Source Initiative: <https://opensource.org/license/mit>
- SPDX identifier: <https://spdx.org/licenses/MIT.html>

See also:

- `../LICENSE`
- `../README.md`

## Purpose

This directory contains small, focused programs that demonstrate how to use the public `libsdrgg` API from user space.

The examples are intended to show:

- device enumeration
- tuner capability introspection
- direct register access
- asynchronous streaming with callback delivery
- synchronous capture using `read_sync`
- tuner-specific gain control
- chip-aware configuration using runtime capability queries
- writing raw IQ data to disk

## Build

From the `libsdrgg` directory:

```bash
make examples
```

Generated binaries:

- `enumerate_devices`
- `show_capabilities`
- `register_access`
- `stream_capture`
- `read_sync_capture`
- `gain_control`
- `save_iq_u8`
- `chip_aware_device`

## Runtime Notes

These examples talk directly to RTL2832U-class USB devices. On many systems you need either:

- `sudo`
- proper udev permissions

Typical usage form:

```bash
sudo ./examples/<example_name>
```

## Example Overview

### `enumerate_devices.cpp`

Lists connected RTL2832U-class devices and prints detected tuner type, path, VID, PID, and serial string.

Useful for:

- verifying USB visibility
- checking tuner detection
- choosing a device index before running other examples

### `show_capabilities.cpp`

Prints `tuner_caps` descriptors for all modeled tuner families, including not-yet-implemented codecs such as FC0013, FC2580, and E4000.

Useful for:

- UI design
- feature discovery
- validating capability metadata

### `register_access.cpp`

Demonstrates raw tuner register reads with `tuner::read_reg()`. Detects the tuner type at runtime and reads chip-appropriate registers:

- R820T/R820T2: register 0x05, signal indices, PLL lock status
- FC0012: chip ID, LNA gain, VCO calibration registers

Useful for:

- low-level diagnostics
- reverse engineering
- quick sanity checks of tuner state

### `stream_capture.cpp`

Queries tuner capabilities to select a valid frequency and gain, then starts asynchronous streaming using `sdr::start_stream()` and counts incoming buffers and bytes through a callback. Validates all return codes before streaming.

Useful for:

- validating async stream setup on any tuner
- measuring raw throughput
- integrating callback-based processing

### `read_sync_capture.cpp`

Uses `sdr::get_tuner_caps()` to choose a frequency within the detected tuner's range, then shows synchronous IQ reads using `sdr::read_sync()` and prints the size and first IQ pair of each block.

Useful for:

- simple blocking capture tools
- debugging without callback/event-loop logic in user code
- quick command-line experiments on any supported chip

### `gain_control.cpp`

Demonstrates chip-specific gain control:

- R820T/R820T2: separate LNA, mixer, VGA, plus bandwidth configuration
- FC0012: gain selection from the discrete levels returned by `fc0012::get_gains()`

Useful for:

- learning tuner-specific APIs
- comparing gain workflows across supported chips

### `save_iq_u8.cpp`

Captures IQ data asynchronously and saves it to an unsigned 8-bit interleaved IQ file such as `capture.u8`.

Default behavior:

- output file: `capture.u8`
- frequency: auto-selected based on tuner capabilities (1090 > 868 > 433 MHz)
- duration: `3 seconds`
- gain: 70% of tuner maximum

Usage:

```bash
sudo ./examples/save_iq_u8 [output_file] [freq_mhz] [seconds]
```

Useful for:

- generating files for offline DSP analysis
- recording RF snapshots
- feeding other SDR tools that accept raw `u8` IQ

### `chip_aware_device.cpp`

Queries `sdr::get_tuner_caps(dev, &caps)` on a real device, chooses a compatible frequency, configures sample rate, and applies chip-appropriate gain setup.

Useful for:

- building chip-aware frontends
- auto-configuration based on detected hardware
- showing how capability metadata can drive runtime policy

## Suggested Usage Sequence

If you are new to the library, a sensible order is:

1. `enumerate_devices`
2. `show_capabilities`
3. `register_access`
4. `chip_aware_device`
5. `read_sync_capture` or `stream_capture`
6. `save_iq_u8`

## Notes

- These examples are intentionally small and explicit rather than wrapped in utility helpers.
- They are meant to be copied, edited, and adapted for experiments.
- Hardware access may fail if another driver or process already owns the dongle.