# LotusGB

LotusGB is a C++ Game Boy emulator and RetroArch libretro core, developed as a
learning project.

## Current status

LotusGB runs a headless DMG system through a libretro core. It supports
ROM-only cartridges and MBC1, MBC2, MBC3 and MBC5, with video, all four DMG
audio channels, joypad input, battery RAM and MBC3 RTC memory. Startup skips
boot-ROM execution and uses a DMG post-boot profile. The PPU renders the
background, window and objects, but some raster timing and BG/OBJ bus behavior
remain approximate. CGB, other mapper families, boot-ROM execution, link play
and MBC5 rumble output are not implemented.

See the [DMG implementation and validation matrix](docs/dmg-validation.md) for
feature and evidence details. The [roadmap](docs/roadmap.md) tracks the active
PPU work and delivery targets; the unresolved Mode 3 behavior is described in
the [PPU guide](docs/ppu-scrolling.md).

## Build and run checks

Requirements: GNU Make, GCC/G++, and Python 3 for generated decoder tables.
The full `make build` also builds `gbit_test`, so keep the pinned GBIT fork
in the sibling `../gbit` directory.

```sh
make build
```

The libretro core is written to `build/lotusgb_libretro.so`. To run the
in-repository CPU, hardware and libretro tests:

```sh
make test
```

These tests do not require GBIT or downloaded ROMs. Optional external ROM
workflows and their prerequisites are documented in the
[validation matrix](docs/dmg-validation.md): Blargg APU checks use
`make test-roms`, Mooneye checks use `make test-mooneye`, and the local homebrew
profiles use `make test-homebrew`. Their ROM files stay outside the repository
unless a ROM's license and attribution permit redistribution.

GitHub Actions builds LotusGB, runs `make test`, and runs instruction checks
against a pinned revision of the
[LotusGB GBIT fork](https://github.com/jiayeli-hualien/gbit).
The CI pin is `40c5247b818a7c524e3c3e75fe0fde900a4ee8e9`. For a local run, use
the same revision in a sibling `../gbit` checkout:

### GBIT instruction integration check

```sh
make -C ../gbit
make build
(cd build && LD_LIBRARY_PATH="../../gbit:.${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ./gbit_test)
```

Run `make -C ../gbit test` for GBIT's own reference/tester checks. LotusGB's
runner stops on the first instruction mismatch; a successful full run reports
244/244 ordinary opcodes and 256/256 CB opcodes.

## What comes next

The active goal is to improve compatibility with a small set of common DMG
games. Use the existing homebrew profiles and user-observed game behavior to
find concrete gaps, then add a focused regression for each confirmed issue.
The investigated SCX raster boundary is paused until there is a model-matched
hardware result or an independently hardware-backed expectation. Selected
Mooneye timing cases and DMG Acid2 validate specific behaviors; they do not
establish complete hardware accuracy. See the
[roadmap](docs/roadmap.md) and [DMG validation matrix](docs/dmg-validation.md).

The platform route is Ubuntu RetroArch first, then Raspberry Pi with Recalbox,
Steam Deck, and Windows. The roadmap defines the acceptance goals and keeps
platform work behind core behavior.

## Project documents

- [Architecture and development workflow](docs/architecture.md)
- [DMG implementation and validation matrix](docs/dmg-validation.md)
- [PPU mid-scanline scrolling and visual checks](docs/ppu-scrolling.md)
- [Interrupt behavior and validation](docs/interrupts.md)
- [Homebrew smoke checks](docs/homebrew-check.md)
- [RetroArch troubleshooting](docs/runtime-troubleshooting.md)
- [Development roadmap](docs/roadmap.md)
- [Documentation writing guide](docs/documentation-guide.md)
- [Reference catalog](docs/references.md)

## Early project history

- **2023-02-12:** Started the project, added a GBIT example, and began a C/C++
  wrapper.
- **2023-02-15:** Integrated GBIT; the initial NOP test did not pass, prompting
  plans for state accessors in the adapter.

## License

This project is licensed under the [MIT License](LICENSE).
