# LotusGB

LotusGB is a C++ Game Boy / Game Boy Color emulator and RetroArch libretro core, developed as a
learning project.

Instant save/load now works through libretro. The [save-state guide](docs/save-states.md) covers its format, RTC behavior and validation scope.

## Current status

LotusGB runs DMG and native CGB games through a libretro core. It supports
ROM-only cartridges and MBC1, MBC2, MBC3/MBC30 and MBC5, video, four audio channels,
joypad input, battery RAM, MBC3 RTC memory and snapshots/rewind. Both `.gb`
and `.gbc` are advertised to RetroArch; the cartridge header selects the model.
DMG startup can use an optional external boot ROM. Native CGB uses fast
post-boot startup without a BIOS.

The first [CGB playability checkpoint](docs/cgb-development.md) passes with
Tobu Tobu Girl Deluxe: color gameplay, input and exact state/PCM replay.
`cgb-acid2` v1.1 matches its official reference in both output formats.
Some raster, DMA and revision-specific behavior remains approximate; broader
GBC game compatibility, connected link/IR and rumble are not established.

See the [DMG implementation and validation matrix](docs/dmg-validation.md) for
feature and evidence details. The [roadmap](docs/roadmap.md) records the
completed personal-use DMG/libretro baseline and future delivery decisions;
unresolved Mode 3 behavior is described in the [PPU guide](docs/ppu-scrolling.md).

## Build and checks

Requirements: GNU Make, a C++17 compiler (GCC or Clang), and Python 3 for
generated decoder tables. `make build` also builds `gbit_test`, so keep the
pinned GBIT fork in the sibling `../gbit` directory.

```sh
make build
```

The libretro core is written to `build/lotusgb_libretro.so`; macOS also
produces `build/lotusgb_libretro.dylib`. `make test` runs the in-repository
CPU, hardware and libretro tests without GBIT or downloaded ROM suites.
The CI-equivalent local gate, GBIT instructions, and optional behavior-specific
ROM checks are centralized in the [testing workflow](docs/testing.md).
See the [macOS development setup](docs/macos-development.md) for platform
details and the [DMG validation matrix](docs/dmg-validation.md) for evidence
and known limits. External ROMs stay outside the repository unless their
terms permit redistribution.

## Current development focus

The bounded M7 headless checkpoint is complete. The default core now returns
at real VBlank completion or a bounded host repeat, preserving shared hardware
clocks and complete-frame output. Tobu Tobu Girl's tested inputs respond in
call 2 instead of the fixed baseline's call 3. Both policies pass the
audio/state/rewind regression gate. See the [input-latency guide](docs/input-latency.md).
Mac/Deck feel, listening and physical latency are deferred by the user.
`make test-tobu-input` measures the default; `make test-tobu-input-fixed`
retains the earlier diagnostic baseline.
The native CGB-E playable checkpoint now includes color rendering, banked
memory, speed switching, VRAM DMA and version 2 snapshots. See the
[CGB behavior and playability evidence](docs/cgb-development.md).

The Adjustris gameplay-progress milestone is complete: its local regression
verifies a line clear and control of the next piece. The Big2Small save/resume
milestone is also complete: its local regression finishes one puzzle, restores
the SRAM in fresh processes, and checks post-restore input. The Steam Deck
build passed a five-minute device session with normal-feeling audio and saved
progress restored after reopening the game; its hash matches the cross-compiled
candidate. An occasional Deck tearing report is closed for now: it did not
reappear in the latest Deck check, and Mac testing shows no tearing. See the
[roadmap](docs/roadmap.md) and [session record](docs/steam-deck-check.md).
Reproducible gameplay blockers take priority. See the [roadmap](docs/roadmap.md)
and [session procedure](docs/steam-deck-check.md) for its acceptance criteria.

macOS is the current development environment, Ubuntu is the CI platform, and
the bounded Steam Deck checkpoint passed for one identified build. The
personal-use DMG/libretro baseline and bounded M5 save-state checkpoints are
complete. Automated save-state checks and macOS RetroArch acceptance pass;
Steam Deck same-session and full-relaunch restore/control pass by user report.
See the [save-state guide](docs/save-states.md) and
[Deck record](docs/steam-deck-check.md) for evidence and identity limits.
The intermittent tearing report is closed for now without a
confirmed core defect. Detailed implementation and validation limits remain
in the [DMG validation matrix](docs/dmg-validation.md).

## Project documents

- [Standard testing workflow](docs/testing.md)
- [Architecture and development workflow](docs/architecture.md)
- [DMG implementation and validation matrix](docs/dmg-validation.md)
- [CGB development and acceptance](docs/cgb-development.md)
- [PPU mid-scanline scrolling and visual checks](docs/ppu-scrolling.md)
- [Interrupt behavior and validation](docs/interrupts.md)
- [Homebrew smoke checks](docs/homebrew-check.md)
- [RetroArch troubleshooting](docs/runtime-troubleshooting.md)
- [Raspberry Pi / Recalbox validation](docs/recalbox.md)
- [Development roadmap](docs/roadmap.md)
- [DMG save states](docs/save-states.md)
- [RetroArch rewind acceptance](docs/rewind.md)
- [Input response and gameplay acceptance](docs/input-latency.md)
- [Steam Deck M3 session procedure](docs/steam-deck-check.md)
- [Documentation writing guide](docs/documentation-guide.md)
- [Reference catalog](docs/references.md)

## Early project history

- **2023-02-12:** Started the project, added a GBIT example, and began a C/C++
  wrapper.
- **2023-02-15:** Integrated GBIT; the initial NOP test did not pass, prompting
  plans for state accessors in the adapter.

## License

This project is licensed under the [MIT License](LICENSE).

For a shareable macOS core installer, run `make package-retroarch-macos`.
The [Universal ZIP](docs/macos-package.md) installs the core and matching
`.info` into an existing RetroArch setup, with backups and info-cache refresh.
The Linux x86-64 Steam Deck package is built with
`make package-retroarch-steam-deck`; `make build TARGET=steamdeck` builds only
the cross-target core and metadata. Plain `make build` and `make test` use the
local Ubuntu x86-64 or Mac M1 host target.

For the x86-64 Linux core package for Steam Deck, see
[the Steam Deck check and installation notes](docs/steam-deck-check.md).
