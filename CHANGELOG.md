# libsdrgg Changelog

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
