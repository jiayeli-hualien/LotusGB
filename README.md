# LotusGB

LotusGB is a C++ emulator project with a RetroArch libretro core for Game Boy
(DMG) and Game Boy Color (CGB). The repository also contains **LotusGBA**, a
separate, experimental Game Boy Advance core. The project is developed as a
learning and personal-use project.

## Status

The bounded single-player target for Pokémon Crystal on CGB-E has passed user
acceptance, including gameplay, in-game saving, and save persistence after
reopening RetroArch. This is a specific playability checkpoint, not a claim of
complete compatibility with every GB/GBC game or full hardware accuracy.
DMG/CGB implementation and evidence are in the
[DMG validation matrix](docs/dmg-validation.md) and
[CGB development record](docs/cgb-development.md).

LotusGBA has automated homebrew gameplay coverage and a private Pokémon
Emerald early-game checkpoint: Route 102 movement, in-game save/reload, and RTC
advancement have been observed. Its video, audio, save, BIOS startup and timing
coverage remain bounded; these results do not establish broad GBA compatibility
or hardware-level fidelity. See the [GBA development record](docs/gba-development.md).

The libretro core has a custom ordered serial-byte stream over RetroArch
Netpacket. One Red/Blue LAN trade and save readback have passed by user report;
link-battle state synchronization remains under investigation. A synthetic
shared-RNG battle and 512-byte payload probe now checks these paths without
commercial game files. A separate two-process stress test verifies 4,096
ordered Netpacket bytes per peer, exceeding the 3,202 paired transfers seen
before a real battle divergence. See the [serial-link design and evidence](docs/serial-link.md).
Public checks use licensed homebrew and original fixtures; commercial ROMs,
derived saves and captures are not included.

## Build and checks

Requirements: GNU Make, a C++17 compiler (GCC or Clang), Python 3, and the
pinned GBIT checkout in the sibling `../gbit` directory. From the repository
root:

```sh
make build
make test
```

`make test` runs the repository's CPU, hardware, system, libretro, framebuffer,
homebrew and GBA checks. The CI-equivalent commands, optional ROM checks and
their limits are described in the [testing workflow](docs/testing.md). Build
outputs include `build/lotusgb_libretro.so` and `build/lotusgba_libretro.so`;
macOS builds also produce `.dylib` variants. See the
[macOS setup](docs/macos-development.md) for host-specific details.

## Documentation

- [Development roadmap](docs/roadmap.md)
- [Current architecture](docs/architecture.md)
- [Testing workflow and ROM policy](docs/testing.md)
- [DMG validation matrix](docs/dmg-validation.md)
- [CGB development and acceptance](docs/cgb-development.md)
- [GBA implementation and evidence](docs/gba-development.md)
- [Homebrew gameplay checks](docs/homebrew-check.md)
- [Serial-link design and evidence](docs/serial-link.md)
- [Save-state format and limits](docs/save-states.md)
- [Input and presentation behavior](docs/input-latency.md)
- [Documentation writing guide](docs/documentation-guide.md)
- [macOS RetroArch packaging](docs/macos-package.md)

This project is licensed under the [MIT License](LICENSE). ROMs and other
third-party test assets retain their own terms; see each asset's README and
license.
