# Libretro adapter

`include/libretro.h` is the official libretro API header from `libretro/RetroArch` commit `ae6ae5ee260aea5390317f5d60a63232416bd1ed`.

- Source: https://github.com/libretro/RetroArch/blob/ae6ae5ee260aea5390317f5d60a63232416bd1ed/libretro-common/include/libretro.h
- SHA-256 of the vendored copy: `7cc38a8be47e337045a7d56020fab68de63f9fee7a78d09b043cba6a40abe81e`.
- One trailing space was removed from the upstream file; the header carries the upstream MIT license notice.

Build the shared core from the repository root with `make build`. The adapter is built from `libretro_core.cpp` and the emulator's system sources; GBIT adapter sources are excluded. It has no runtime dependency on a system-installed RetroArch or libretro-common package.

The default adapter uses bounded VBlank presentation: submit a completed
frame at its event, or repeat the stable/blank image when the 70,224-T-cycle
budget expires. CPU STOP and errors remain observable. Audio always follows
actual shared-clock progress. `make test` runs this policy and the
`LOTUSGB_TEST_FIXED_FRAME_STEP` diagnostic baseline. See the
[presentation policy and evidence](../../docs/input-latency.md#current-presentation-policy).

The core advertises `gb|gbc` in `retro_get_system_info` and the matching `.info`,
including both Game Boy databases. The header CGB flag selects native CGB for
both extensions. Native CGB uses fast post-boot startup without a BIOS; DMG
retains its optional BIOS loader. Both XRGB8888 and 0RGB1555 receive color
conversion from the CGB RGB555 framebuffer. Snapshots are v1 for DMG and v2
for CGB. See [CGB behavior and evidence](../../docs/cgb-development.md).

On macOS, `make install-retroarch-core-macos` installs the library and `.info`
together and creates `core_info.refresh` for the next RetroArch startup.
Override `RETROARCH_CORE_DIR` / `RETROARCH_INFO_DIR` when needed. On Deck,
install the `.so` and the updated `.info` using the existing
[device procedure](../../docs/steam-deck-check.md); replacing only the library
can leave `.gbc` content filtering or state features cached from an older info.

`make package-retroarch-macos` builds a separate Universal core installer ZIP
for existing macOS RetroArch installations. See the
[installer and validation guide](../../docs/macos-package.md).
