# Libretro adapter

`include/libretro.h` is the official libretro API header from `libretro/RetroArch` commit `ae6ae5ee260aea5390317f5d60a63232416bd1ed`.

- Source: https://github.com/libretro/RetroArch/blob/ae6ae5ee260aea5390317f5d60a63232416bd1ed/libretro-common/include/libretro.h
- SHA-256 of the vendored copy: `7cc38a8be47e337045a7d56020fab68de63f9fee7a78d09b043cba6a40abe81e`.
- One trailing space was removed from the upstream file; the header carries the upstream MIT license notice.

Build the shared core from the repository root with `make build`. The adapter is built from `libretro_core.cpp` and the emulator's system sources; GBIT adapter sources are excluded. It has no runtime dependency on a system-installed RetroArch or libretro-common package.
