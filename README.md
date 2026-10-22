# LotusGB

LotusGB is a learning project to build a Nintendo Game Boy emulator in C++.

## Project status

The repository contains a CPU implementation, a headless DMG system, and a
libretro adapter for RetroArch. It supports ROM-only games up to 32 KiB,
MBC1 cartridge types `$01`–`$03`, and MBC3 types `$11`–`$13` with ROM banking
up to 2 MiB and up to 32 KiB of external RAM. MBC3 timer variants are not
supported yet. Battery-backed RAM is exposed through libretro's save RAM
interface for RetroArch to persist. The core also supports joypad input, a
partial DMG PPU, and all four DMG APU channels. The default startup profile
skips the boot ROM. Other cartridge mappers, boot-ROM execution, and more exact
PPU/APU timing remain future work.

CPU behavior and timing are covered by focused tests and GBIT integration
checks. The CPU advances in M-cycle steps, and the system clock distributes
elapsed T-cycles to the hardware components.

## Roadmap

### Near term

- Refine DMG audio and visual compatibility with focused tests and reference
  games; F-1 Race is a PPU visual target.
- Broaden cartridge support. The libretro test simulates restoring MBC3 save
  RAM and a game reading and updating that progress; verify disk persistence
  with a legally available MBC3 game that stores progress.
- Maintain GitHub Actions CI for the build, unit tests, and GBIT instruction
  tests.

### Longer term

- Verify the libretro core on Steam Deck and Raspberry Pi with Recalbox.
- Add Windows build and CI coverage after the core behavior is established.

## Building

The project uses GNU Make and GCC/G++. From the repository root, run:

```sh
make build
```

The opcode CSV files in `src/lib/include` are the editable source for the
decoder tables. The build converts their fields into typed C++ tables in a
generated header; the program does not parse CSV or open CSV files at runtime.
Python 3 is needed to regenerate that header when a CSV changes.

Run the in-repository unit tests with:

```sh
make test
```

These tests cover CPU interfaces and instructions, hardware components, and
the libretro adapter. They do not require GBIT.

### Optional Blargg APU ROM checks

Blargg ROMs stay outside the repository. If `../test-roms/dmg_sound` contains
the DMG sound ROMs, run the selected CH3 checks with:

```sh
make test-roms
```

Set `TEST_ROM_DIR=/path/to/dmg_sound` to use another directory, or change the
run length with `TEST_ROM_FRAMES=1200`. The command checks each original ROM's
SHA-256, preserves its MBC1 header, and runs the built libretro core for a fixed
number of frames. It saves screenshots, SRAM dumps, logs, and an automatically
checked pass/fail report under `build/test-roms/`. Missing ROMs are reported as
skipped. The ROM binaries remain outside the repository; these checks are not
part of GitHub CI.

The `gbit_test` target expects your [GBIT fork](https://github.com/jiayeli-hualien/gbit)
to be checked out as a sibling directory next to `lotus-gbemu`. The current
`application` target is still a placeholder. After building, run the GBIT
instruction checks from the build directory:

```sh
cd build
LD_LIBRARY_PATH="../../gbit:.${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ./gbit_test
```

## History

- **2023-02-12:** Started the project, added an example using GBIT, and began a
  C/C++ wrapper.
- **2023-02-15:** Integrated GBIT; the initial NOP test did not pass. Planned
  state accessors for the GBIT adapter.

## References

### Game Boy development and hardware

- [Pan Docs](https://gbdev.io/pandocs/) — community-maintained Game Boy hardware reference ([about and license](https://eldred.fr/pandocs/About.html))
- [Pan Docs: Audio Registers](https://gbdev.io/pandocs/Audio_Registers.html) — APU register behavior and wave RAM access
- [Pan Docs: Audio Details](https://gbdev.io/pandocs/Audio_details.html) — channel sequencing, noise LFSR behavior, and output filtering
- [Pan Docs: MBCs](https://gbdev.io/pandocs/MBCs.html) — cartridge banking and mapper behavior
- [Pan Docs: The Cartridge Header](https://gbdev.io/pandocs/The_Cartridge_Header.html) — cartridge types and ROM/RAM size codes
- [MBC3 Bank Tester](https://github.com/EricKirschenmann/MBC3-Tester-gb) — mapper bank diagnostic; its 4 MiB ROM and screenshot-based result check do not cover frontend save-file persistence ([test procedure](https://github.com/c-sp/game-boy-test-roms/blob/master/src/howto/mbc3-tester.md))
- [Game Boy sound hardware (GbdevWiki)](https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware) — register layout, trigger behavior, and observed hardware differences
- [SameBoy APU implementation](https://github.com/LIJI32/SameBoy/blob/master/Core/apu.c) — implementation comparison for channel timing and LFSR behavior
- [Blargg DMG sound tests](https://github.com/retrio/gb-test-roms/tree/master/dmg_sound) — CPU-visible register, length, trigger, and CH3 wave RAM behavior tests
- [Blargg test ROM archive](https://gbdev.gg8.se/files/roms/blargg-gb-tests/) — public downloadable test packs, including `dmg_sound.zip`
- [Blargg DMG sound test sources (reproducible mirror)](https://github.com/crzysdrs/blarggs-test-roms/tree/master/dmg_sound) — assembly sources and expected checksums for individual tests
- [Game Boy Noise Test](https://github.com/not-chciken/gb-noise-test) — interactive CH4 settings and presets with DMG-01 audio reference recordings (Apache-2.0)
- [SameSuite APU tests](https://github.com/LIJI32/SameSuite/tree/master/apu) — sample-accurate APU tests; most rely on CGB-only PCM registers and do not apply to a DMG core
- [Game Boy: Complete Technical Reference](https://github.com/Gekkio/gb-ctr)
- [Building a Game Boy emulator](https://wwssllabcd.github.io/2021/02/08/how-to-build-a-gameboy-emulator-1/)
- [Game Boy emulator notes (HackMD)](https://hackmd.io/@sysprog/BJvOWzFqv)
- [Game Boy emulator notes by RinHizakura](https://hackmd.io/@RinHizakura/BJ6HoW29v)
- [The Ultimate Game Boy Talk](https://media.ccc.de/v/33c3-8029-the_ultimate_game_boy_talk)
- [Thomas Spurden's Game Boy notes](https://thomas.spurden.name/gameboy)

### Project and build references

- [GBIT](https://github.com/koenk/gbit)
- [Calling C++ from C](http://www.huoschen.idv.tw/blog/programming/c-calling-c++-functions/)
- [Creating a shared library with Make](https://stackoverflow.com/questions/8096015/creating-a-simple-makefile-to-build-a-shared-library)
- [GNU Make notes](https://jasonblog.github.io/note/gunmake/gunmake.html)
- [Makefile example](https://gist.github.com/wenchy/64db1636845a3da0c4c7)

## License

This project is licensed under the [MIT License](LICENSE).
