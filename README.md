# LotusGB

Play Game Boy and Game Boy Color games in RetroArch with **LotusGB**. It also
supports link play between two systems over a network. The project includes
**LotusGBA**, an experimental Game Boy Advance core.

The project prioritizes game compatibility and a smooth play experience.
Hardware behavior guides the implementation, but a tested, documented
compatibility behavior may intentionally differ from the original hardware.
See the [development roadmap](docs/roadmap.md#project-objective-and-fidelity-policy)
for this policy and the evidence behind current support claims.

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
or hardware-level fidelity. See the [GBA implementation status](docs/gba-status.md).

LotusGBA also has experimental Wireless Adapter and Link Cable network play
for a limited set of Pokémon Emerald versions. See the
[LotusGBA link play guide](docs/gba-link-play.md) for device selection,
compatibility, save precautions and connection steps.

The LotusGB libretro core sends ordered link data through RetroArch Netpacket. Pokémon
Red/Blue LAN trade and battle checks have completed in reported setups, though
these bounded results do not guarantee every game, session, or network will
stay in sync. Synthetic tests also check shared battle RNG and long ordered
data streams without commercial game files. See the
[serial-link design and evidence](docs/serial-link-overview.md) for results and limits.
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
- [GBA implementation and evidence](docs/gba-status.md)
- [LotusGBA link play in RetroArch](docs/gba-link-play.md)
- [Homebrew gameplay checks](docs/homebrew-check.md)
- [Serial-link design and evidence](docs/serial-link-overview.md)
- [Save-state format and limits](docs/save-states.md)
- [Input and presentation behavior](docs/input-latency.md)
- [Documentation writing guide](docs/documentation-guide.md)
- [macOS RetroArch packaging](docs/macos-package.md)

This project is licensed under the [MIT License](LICENSE). ROMs and other
third-party test assets retain their own terms; see each asset's README and
license. LotusGB is an unofficial, independently developed project; game
titles are referenced only to describe compatibility testing. No commercial
game files are included.
