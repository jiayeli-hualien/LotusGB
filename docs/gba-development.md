# GBA development

## First-stage target

LotusGBA is a separate libretro core in the LotusGB repository. It has its own
ARM7TDMI CPU, GBA memory map, video state and frontend adapter. It does not
share the GB/CGB CPU or `GameBoySystem`; only the repository build, test and
packaging workflows are shared.

The first playable target is
[Pong-Homebrew-GBA](https://github.com/ZeroDayArcade/Pong-Homebrew-GBA), a
Mode 3 game. Its ROM and MIT license are in `tests/roms/gba-pong/`; the test
copy has the unlicensed Nintendo logo bytes cleared. The libretro callback
test reaches the title menu, starts a match, observes ball movement, moves the
human paddle with Up and Down, verifies a paddle collision and ball return,
and reaches the first CPU score. This is an automated gameplay checkpoint,
not a manual RetroArch or hardware acceptance.

The instruction gate also covers the licensed ARM and Thumb suites from
`jsmolka/gba-tests`. Both display “All tests passed” in Mode 4, and the
callback test checks the success glyph for each. Their ROMs and licenses are
under `tests/roms/gba-arm-tests/` and `tests/roms/gba-thumb-tests/`. Nintendo
logo header bytes were cleared because the upstream MIT license does not
separately license that asset. Original fixture sources are in
`tests/include/gba_boot_rom.hpp` and are covered by the repository MIT license.

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
  IWRAM. No proprietary BIOS is bundled or required for the tested game.
- The bus maps ROM, EWRAM, IWRAM, I/O, palette RAM, VRAM and OAM. The current
  video path renders direct-color bitmap Mode 3, indexed bitmap Mode 4 and
  forced blank.
- CPU mode switching banks FIQ, IRQ, SVC, Abort and Undefined registers and
  saved status. The HLE IRQ vector calls the cartridge handler at
  `0x03007FFC`, preserves the caller state and returns from the exception.
- VBlank status and IRQ requests, IE/IF/IME registers, keypad input and the
  `IntrWait`/`VBlankIntrWait`/`Halt` BIOS calls support Pong's frame loop.
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
execution, accurate wait states and instruction timing, timers and non-VBlank
interrupt sources, DMA, audio, tile and affine video modes, save hardware and
state serialization remain unimplemented. Pong's automated callbacks establish
the tested menu, control, ball and score path; they do not establish broad GBA
compatibility or manual frontend acceptance.

## Next implementation gates

1. Use the Pong callback regression to keep the playable vertical slice intact.
2. Add named, licensed game targets before expanding beyond Pong; implement
   only the missing hardware each target demonstrates, with focused timing and
   component tests.
