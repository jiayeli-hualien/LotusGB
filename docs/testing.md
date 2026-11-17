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
Ubuntu x86-64 or Apple Silicon macOS.

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
`cgb_compatibility_test` checks boot-selected KEY0/OPRI locking, register/bank
restrictions, monochrome palette rendering on CGB and pre/post-lock state replay.
Libretro covers hardware-option fallback/selection, reset and exact compatibility
video/PCM/state rewind in both pixel/presentation formats.
`cgb_memory_test` checks the CGB memory components with an original CPU probe;
`cgb_boot_test` checks explicit CGB-E assembly, split boot handoff and bank
retention through `GameBoySystem`, plus the synthetic fixture factory's LCD-off/snapshot restriction (separate from playable CGB startup).
`ppu_video_memory_test` compares owned/external VRAM with the monochrome
renderer. `cgb_hardware_test` checks palette ports, DMA and fast serial/divider
behavior plus all-mode speed-switch PPU locks and output; `cgb_system_test`
checks exact cgb-acid2 pixels, speed/DMA/audio/input, real KEY1/STOP pauses in
both directions for all four PPU entry modes,
model-tagged state replay and invalid typed payload rejection. Libretro tests
check the CGB reference, input/reset and exact video/PCM/state rewind in both
pixel formats. Original CGB-compatible MBC1 and MBC5 game fixtures additionally
check color/input gameplay, battery progress and fresh-instance SRAM restore.
An original CGB MBC3+RTC fixture exercises guest RTC latch/read/write and checks
RTC plus SRAM import across a fresh content load in both presentation builds.
`dmg_apu_test` also checks every digital level through each channel DAC on both
models, NR50 minimum/maximum gain, inactive-generator bias and DAC isolation.
Envelope tests cover boundary-stop/retrigger behavior and period-zero divider
phase; `save_state_test` drives both models through a CPU/DIV terminal-envelope
checkpoint and exact PCM/state replay. Silent-pulse tests cover all duty
patterns, disable causes and retrigger phase; `cgb_system_test` compares guest
PCM against a continuously clocked reference and restores mid-silence.
`mbc30_test` and libretro MBC30 fixtures also cover 4-MiB ROM / 64-KiB SRAM,
RTC, battery import and high-bank state replay using original code.
The optional `make test-tobudx` runs the pinned external Deluxe
ROM; it is separate from CI. These checks do not imply all CGB hardware timing
or whole-library compatibility. See the
[CGB contracts and focused command](cgb-development.md#reproducible-checks).
The libretro tests also print the [input-response baseline](input-latency.md):
GB Pong press/release callback counts and synthetic one-poll input checks in
both pixel formats. These do not measure physical controller/display latency.

The Netpacket regression in `make test` runs two core processes on the same
machine using Unix socket pairs. GitHub's Ubuntu runner runs it without
RetroArch or an IP network connection. Both probe ROMs first arm external
clock and must neither send packets nor complete a transfer. The harness
then lets one ROM select internal clock and checks that the ROMs receive
complementary role bytes and exchange the full ordered payload. The short
and long runs give the clock to opposite Netpacket peers.

The GBIT comparison should report 244/244 ordinary opcodes and 256/256 CB
opcodes. The standard sequence was run on macOS on 2026-10-28: GBIT's own
regressions, `make build`, `make test`, and the LotusGB GBIT comparison all
passed.

## Reuse one Linux core for Ubuntu and Steam Deck

Cross-compile once per source revision, then copy the same Linux core to both
machines. On Apple Silicon macOS, build with:

~~~sh
make build TARGET=steamdeck
file build/steamdeck/lotusgb_libretro.so
shasum -a 256 build/steamdeck/lotusgb_libretro.so
~~~

The target name is `steamdeck`, and its default Zig target is
`x86_64-linux-gnu`. The output is a dynamically linked Linux x86-64 ELF
library. Each destination needs an x86-64 RetroArch process and a Linux
runtime that satisfies the library's dependencies, including its glibc
symbol versions. macOS RetroArch loads a separate native Mach-O core.

On Ubuntu and Steam Deck, check the SHA-256 of the file RetroArch will
actually load with `sha256sum /path/to/lotusgb_libretro.so`. Confirm the core
path in the RetroArch startup log and, for LAN tests, confirm the same
Netpacket protocol version on both peers. This keeps the core build fixed
while comparing the two environments.

The Zig build path has passed gameplay checks on both distributions.
The [Steam Deck checkpoint](steam-deck-check.md#m3-gameplay-and-battery-save-checkpoint--passed)
records a hash-identified Linux core. The [Red/Blue LAN rematch](serial-link.md)
used a later Mac-built Linux core on Ubuntu RetroArch 1.18.0. In the Ubuntu
run, the deployed core's SHA-256 matched the Mac build output. The two
checkpoints used different revisions, so a new build still needs a load
check on each destination.

The default toolchain is `build/toolchains/zig-0.17.0/zig`. Override it
with `STEAM_DECK_ZIG=/path/to/zig` or change the target triple with
`STEAM_DECK_TARGET`. Use `make package-retroarch-steam-deck` for the install
ZIP. Cross-target builds are compile/package only, and
`make test TARGET=steamdeck` is rejected. Use
`make package-retroarch-macos` with macOS/Xcode tools for the native universal
package.

## Add only the checks relevant to the change

| Change area | Additional check | Acceptance evidence |
| --- | --- | --- |
| CPU instruction semantics or timing | Pinned GBIT comparison; the focused CPU tests are part of `make test` | Instruction result and the relevant public state, bus, or cycle boundary. GBIT agreement is a comparison, not hardware proof. |
| CPU, timer, interrupt, serial, DMA, mapper, or PPU behavior | Matching Mooneye group, if the external suite is available: `cpu`, `ppu` (default), `timer`, `interrupt` (including `interrupts/ie_push`), `serial`, `dma`, `mapper` (including standard MBC1M), `cgb` (synthetic CGB-E startup), or `cgb-boot` (external CGB BIOS required) | Record the ROM, target model, startup profile, and expected pass signal. Use only the group relevant to the behavior; reserve `all` for a broad checkpoint. |
| Libretro input, audio, save, or frame delivery | Relevant in-process libretro test; add a licensed game profile when the behavior reaches gameplay | Check the frontend-visible result, not callback counts alone. Review the generated image/report for game runs. |
| DMG sound behavior | `make test-roms` with the selected Blargg ROMs available externally | Record the selected cases and their reported result; these cover specific APU paths, not complete sound accuracy. |
| Exact visual diagnostic | `make test-mooneye-visual` or `make test-gbmicrotest-ppu-latch` only when that scene/register behavior is under investigation | Inspect the generated image and keep the result scoped to the diagnostic. A cross-core image difference is not a hardware verdict. |

Mooneye examples:

```sh
make test-mooneye MOONEYE_GROUP=cpu MOONEYE_SUITE_DIR=../test-roms/mooneye
make test-mooneye MOONEYE_GROUP=timer MOONEYE_SUITE_DIR=../test-roms/mooneye
make test-mooneye MOONEYE_GROUP=interrupt MOONEYE_SUITE_DIR=../test-roms/mooneye
make test-mooneye MOONEYE_GROUP=cgb MOONEYE_SUITE_DIR=../test-roms/mooneye
make test-mooneye MOONEYE_GROUP=cgb-boot MOONEYE_CGB_BOOT_ROM=/path/to/cgb_boot.bin MOONEYE_SUITE_DIR=../test-roms/mooneye
```

The optional `cgb` group runs the CGB-targeted VBlank/STAT timing ROM against
LotusGB's CGB-E model with synthetic post-boot startup. The `cgb-boot` group
runs initial-register and unused-HWIO checks that require an external CGB boot
ROM; set `MOONEYE_CGB_BOOT_ROM` to its 2304-byte address-indexed image. Both
profiles keep external ROMs and firmware outside the repository and are not
part of GitHub CI.

Homebrew profiles currently available in the local setup:

```sh
make test-homebrew
make test-cgb-homebrew
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
inspection read-only and skips the fresh-save round trip. Its pinned header is
CGB-compatible and declares MBC5+RAM+battery; `make test-cgb-homebrew` names
this optional local gameplay/save check and the Deluxe CGB/MBC1 input/state
check explicitly. Both ROMs stay external and out of CI and the repository.
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
