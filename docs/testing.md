# LotusGB testing workflow

Use the same required checks as CI for a change intended to merge. Add an
optional ROM or gameplay run only when it exercises the behavior being changed.
Do not run every diagnostic suite for every change.

## Required local gate

CI pins the GBIT fork at
`40c5247b818a7c524e3c3e75fe0fde900a4ee8e9` in the sibling `../gbit`
directory. From the LotusGB repository root, run:

```sh
make -C ../gbit
make -C ../gbit test
make build
make test
```

These default commands build and run tests for the detected local host:
Ubuntu x86-64 or Mac M1. Use `make build TARGET=steamdeck` to cross-compile the Linux x86-64 core; use
`make package-retroarch-steam-deck` to create its install ZIP. Cross-target
builds do not run host tests, and `make test TARGET=steamdeck` is rejected.
The Deck build uses `build/toolchains/zig-0.17.0/zig` by default; override it
with `STEAM_DECK_ZIG=/path/to/zig` and select another Zig target triple with
`STEAM_DECK_TARGET=x86_64-linux-gnu`. macOS Universal packaging remains
`make package-retroarch-macos` and requires macOS/Xcode tools.

Then run LotusGB's GBIT instruction comparison from `build/`:

```sh
(cd build && LD_LIBRARY_PATH="../../gbit:.${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ./gbit_test)
```

Use the same GBIT revision as CI. On macOS, build GBIT with the linker override
and use `DYLD_LIBRARY_PATH`; see the [macOS setup](macos-development.md).
`make build` builds the core and frontend adapter. `make test` runs the CPU,
hardware-component, system, libretro, Acid2 framebuffer, and runner-protocol
tests. The libretro gate runs the ordinary VBlank-presentation and diagnostic fixed-step
builds, including 48 kHz cadence, LCD restart, state restore and repeated
rewind-protocol replay. These gates do not need downloaded ROM suites.
`cgb_memory_test` checks the CGB memory components with an original CPU probe;
`cgb_boot_test` checks explicit CGB-E assembly, split boot handoff and bank
retention through `GameBoySystem`, plus the synthetic fixture factory's LCD-off/snapshot restriction (separate from playable CGB startup).
`ppu_video_memory_test` compares owned/external VRAM with the monochrome
renderer. `cgb_hardware_test` checks palette ports, DMA and fast serial/divider
behavior; `cgb_system_test` checks exact cgb-acid2 pixels, speed/DMA/audio/input,
model-tagged state replay and invalid typed payload rejection. Libretro tests
check the CGB reference, input/reset and exact video/PCM/state rewind in both
pixel formats. `mbc30_test` and libretro MBC30 fixtures additionally cover 4-MiB ROM / 64-KiB
SRAM, RTC, battery import and high-bank state replay using original code.
The optional `make test-tobudx` runs the pinned external Deluxe
ROM; it is separate from CI. These checks do not imply all CGB hardware timing
or whole-library compatibility. See the
[CGB contracts and focused command](cgb-development.md#reproducible-checks).
The libretro tests also print the [input-response baseline](input-latency.md):
GB Pong press/release callback counts and synthetic one-poll input checks in
both pixel formats. These do not measure physical controller/display latency.

The GBIT comparison should report 244/244 ordinary opcodes and 256/256 CB
opcodes. The standard sequence was run on macOS on 2026-10-28: GBIT's own
regressions, `make build`, `make test`, and the LotusGB GBIT comparison all
passed.

## Add only the checks relevant to the change

| Change area | Additional check | Acceptance evidence |
| --- | --- | --- |
| CPU instruction semantics or timing | Pinned GBIT comparison; the focused CPU tests are part of `make test` | Instruction result and the relevant public state, bus, or cycle boundary. GBIT agreement is a comparison, not hardware proof. |
| Timer, interrupt, DMA, mapper, or PPU behavior | Matching Mooneye group, if the external suite is available: `ppu` (default), `timer`, `interrupt`, `dma`, or `mapper` | Record the ROM, target model, startup profile, and expected pass signal. Use only the group relevant to the behavior; reserve `all` for a broad checkpoint. |
| Libretro input, audio, save, or frame delivery | Relevant in-process libretro test; add a licensed game profile when the behavior reaches gameplay | Check the frontend-visible result, not callback counts alone. Review the generated image/report for game runs. |
| DMG sound behavior | `make test-roms` with the selected Blargg ROMs available externally | Record the selected cases and their reported result; these cover specific APU paths, not complete sound accuracy. |
| Exact visual diagnostic | `make test-mooneye-visual` or `make test-gbmicrotest-ppu-latch` only when that scene/register behavior is under investigation | Inspect the generated image and keep the result scoped to the diagnostic. A cross-core image difference is not a hardware verdict. |

Mooneye examples:

```sh
make test-mooneye MOONEYE_GROUP=timer MOONEYE_SUITE_DIR=../test-roms/mooneye
make test-mooneye MOONEYE_GROUP=interrupt MOONEYE_SUITE_DIR=../test-roms/mooneye
```

Homebrew profiles currently available in the local setup:

```sh
make test-homebrew
make test-homebrew HOMEBREW_GAME=adjustris
make test-homebrew HOMEBREW_GAME=tobutobugirl
make test-homebrew HOMEBREW_GAME=canyon-racer
```

For the selected Tobu Tobu Girl input-response assessment, run
`make test-tobu-input`. Compare it with `make test-tobu-input-fixed`, which
builds the earlier fixed-step diagnostic executable. The older
`make test-tobu-input-vblank` name also tests the new default in a separate
output directory. None installs a core.
Both use the pinned external ROM and compare neutral,
one-poll and held actions from identical snapshots. Results and reviewed-frame
definitions are in the [input-response guide](input-latency.md#tobu-tobu-girl-action-baseline-2026-11-01).
This optional game measurement is not a CI dependency.
PPU completion events and `GameBoySystem::stepUntilVBlank()` timeout,
STOP/HALT, restore and early-LY boundary checks are covered by `make test`.
Use `make test-vblank` for just the default VBlank-presentation libretro gate;
`make test` already includes it. The [headless assessment](input-latency.md#headless-pacing-and-rewind-assessment-2026-11-01)
records the cadence differences; matching PCM to actual hardware time is
checked separately from maintaining a fixed amount of time per video call.

The default profile is Big2Small. The Adjustris check also writes
`progress-full-row.ppm`, `progress-row-cleared.ppm`,
`progress-next-piece-moved.ppm`, and a raw frame sequence beside its report.
The default Big2Small profile completes Peanut Pathway, compares a restored
02% save with a restored 00% slot, and checks directional response after
loading the completed save in a fresh process. It writes all SRAM and captures
under `build/test-roms/big2small/`; `HOMEBREW_SAVE` keeps the supplied-save
inspection read-only and skips the fresh-save round trip.
Canyon Racer is an optional external profile for raster-effect gameplay
investigation; it was not part of the 2026-10-28 consolidated three-profile run.
Inspect the generated views and `report.json`; keep source ROMs, saves and game
data outside the repository unless the ROM's own license and attribution allow
distribution. Commercial ROMs and game-derived results stay outside the
repository and CI.

## What is not part of the routine gate

- Do not run `MOONEYE_GROUP=all`, every homebrew game, Blargg, and visual
  diagnostics for an unrelated code change. Run the relevant subset; use the
  broad Mooneye group for a milestone or a change spanning several devices.
- `make test-mooneye-visual` and `make test-gbmicrotest-ppu-latch` are targeted
  diagnostics, not general pass/fail acceptance suites.
- Do not repeat a game profile when an identical ROM hash, core build, input
  script, and question already have a current result. Repeat when code or
  inputs change, or when checking determinism.

No existing automated test executable was removed in this audit: the CPU,
component, system, libretro, framebuffer, and runner-protocol tests cover
different boundaries. The cleanup is to stop treating optional diagnostic
suites as routine work and to keep their invocation instructions in this one
guide.
