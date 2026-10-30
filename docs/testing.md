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

Then run LotusGB's GBIT instruction comparison from `build/`:

```sh
(cd build && LD_LIBRARY_PATH="../../gbit:.${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ./gbit_test)
```

Use the same GBIT revision as CI. On macOS, build GBIT with the linker override
and use `DYLD_LIBRARY_PATH`; see the [macOS setup](macos-development.md).
`make build` builds the core and frontend adapter. `make test` runs the CPU,
hardware-component, system, libretro, Acid2 framebuffer, and runner-protocol
tests. These gates do not need downloaded ROM suites.

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
