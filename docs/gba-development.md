# GBA development

## First-stage target

LotusGBA is a separate libretro core in the LotusGB repository. It has its own
ARM7TDMI CPU, GBA memory map, video state and frontend adapter. It does not
share the GB/CGB CPU or `GameBoySystem`; only the repository build, test and
packaging workflows are shared.

The boot gate covers two original in-memory cartridges and the licensed ARM
and Thumb instruction suites from `jsmolka/gba-tests`. The fixtures check ARM
startup and an ARM-to-Thumb handoff, then fill Mode 3 with red through the
public libretro callbacks. Both external suites display “All tests passed” in
Mode 4, and the callback test checks the success glyph for each. Their ROMs and
licenses are under `tests/roms/gba-arm-tests/` and
`tests/roms/gba-thumb-tests/`. Nintendo logo header bytes were cleared because
the upstream MIT license does not separately license that asset. Original
fixture sources are in `tests/include/gba_boot_rom.hpp` and are covered by the
repository MIT license.

Build both installed core artifacts from the repository root with:

```sh
make build
```

This produces `build/lotusgba_libretro.so` and its `.info`; on macOS it also
copies the shared object to `build/lotusgba_libretro.dylib`. Run the targeted
boot check with:

```sh
make -C src/gba_libretro build_dir=../../build test
```

The normal `make test` route also runs this GBA core test.

## Implemented behavior

- Libretro identifies itself as `LotusGBA`, accepts in-memory `.gba` content,
  reports 240×160 at the GBA frame rate, reads standard joypad input and submits
  silence while audio hardware is absent.
- Startup uses an HLE handoff to `0x08000000` in ARM state, with a stack in
  IWRAM. No proprietary BIOS is bundled or required for the licensed test.
- The bus maps ROM, EWRAM, IWRAM, I/O, palette RAM, VRAM and OAM. The current
  video path renders direct-color bitmap Mode 3, indexed bitmap Mode 4 and
  forced blank.
- ARM coverage includes the instruction groups exercised by the licensed ARM
  suite: conditions, branches, shifts and data processing; selected CPSR/SPSR
  behavior; multiply; word, byte and halfword transfers; SWP; and block
  transfers. The HLE BIOS implements the Div SWI used to report failures.
- Thumb mode passes the external suite's logical, shift, arithmetic, branch
  and memory groups, including stack operations, multiple transfers and
  interworking. This is test-suite coverage, not proof of every Thumb edge case.
- Each supported instruction currently advances one prototype cycle.

## Known limits

This is a testable GBA prototype, not a general compatibility claim. The
external ARM and Thumb suites pass, but untested instruction edge cases remain;
unsupported instructions stop with an explicit CPU fault. Actual BIOS
execution, accurate wait states and instruction timing, timers, interrupts,
DMA, audio, tile and affine video modes, save hardware, state serialization and
real-game acceptance remain unimplemented. These checks do not establish game
startup or playability.

## Next implementation gates

1. Choose a licensed homebrew as the first game-level GBA target and record
   which boot and play checkpoints define success.
2. Add the minimum BIOS HLE, interrupt, timer, DMA and PPU behavior that target
   requires, guided by named tests and explicit cycle accounting.
3. Expand audio and other hardware behavior against named test ROMs before
   claiming a game is playable.
