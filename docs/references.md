# Reference catalog

These sources inform LotusGB's hardware model, test selection and implementation.
Hardware documentation and hardware-tested ROMs are the primary behavior
references. Other emulator source code is useful for comparison, not as a
hardware specification.

## Hardware behavior

### General references

- [Pan Docs](https://gbdev.io/pandocs/) — community-maintained Game Boy hardware reference ([about and license](https://eldred.fr/pandocs/About.html)).
- [Nintendo Game Boy Programming Manual v1.1](https://archive.org/details/GameBoyProgManVer1.1) — programmer-facing manual for CPU, display, sound and mapper interfaces; it does not describe all internal PPU fetch timing.
- [Game Boy: Complete Technical Reference](https://github.com/Gekkio/gb-ctr) — community hardware reference with model and revision differences; its CPU chapter documents fetch/execute overlap and per-M-cycle IDU actions. The project describes itself as incomplete.
- [The Cycle-Accurate Game Boy Docs](https://github.com/AntonioND/giibiiadvance/blob/master/docs/TCAGBD.pdf) — community hardware notes, including serial clock behavior.
- [Game Boy sound hardware (GbdevWiki)](https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware) — register layout, trigger behavior and observed differences.

### CPU, interrupts, timer and memory

- [Pan Docs: Interrupts](https://gbdev.io/pandocs/Interrupts.html) and [HALT](https://gbdev.io/pandocs/halt.html) — interrupt requests and CPU behavior.
- [Pan Docs: OAM Corruption Bug](https://gbdev.io/pandocs/OAM_Corruption_Bug.html) — DMG OAM effects from memory access and 16-bit IDU operations, including implicit SP decrements during stack pushes.
- [Game Boy CPU internals](https://gist.github.com/SonoSooS/c0055300670d678b5ae8433e20bea595) — reverse-engineered SM83 micro-operation schedules for stack and interrupt entry; useful as a timing reference, not a substitute for hardware verification.
- [GB Interceptor bus observations](https://there.oughta.be/a/game-boy-capture-cartridge) — cartridge-bus traces show interrupt stack writes and model-specific address-bus differences.
- [Pan Docs: Timer obscure behaviour](https://gbdev.io/pandocs/Timer_Obscure_Behaviour.html) — divider edges, overflow delay and TIMA/TMA reload writes.
- [Pan Docs: OAM DMA Transfer](https://gbdev.io/pandocs/OAM_DMA_Transfer.html) — transfer timing and bus conflicts.
- [Pan Docs: Serial Data Transfer](https://gbdev.io/pandocs/Serial_Data_Transfer_%28Link_Cable%29.html) — SB/SC, internal/external clocks and completion interrupt.

### PPU

- [Nesdev discussion: Game Boy CPU and PPU timing](https://forums.nesdev.org/viewtopic.php?t=17754) — high-level explanation of parallel CPU/PPU clocks and mode timing; use for the scheduling model, not the precise fetcher boundary.
- [Stack Overflow: How do GPU/PPU timings work on the Game Boy?](https://stackoverflow.com/questions/52153636/how-do-gpu-ppu-timings-work-on-the-gameboy) — introductory discussion of shared time progression and updating devices around CPU memory accesses; it is not a hardware timing specification.
- [Pan Docs: Rendering](https://gbdev.io/pandocs/Rendering.html) — PPU modes, pixel output and Mode 3 penalties.
- [Pan Docs: Pixel FIFO](https://gbdev.io/pandocs/pixel_fifo.html) — BG/OBJ FIFOs and sprite-fetch interaction; detailed timing still needs independent validation.
- [Pan Docs: Scrolling](https://gbdev.io/pandocs/Scrolling.html) — SCX/SCY sampling behavior.
- [Nitty Gritty Gameboy Cycle Timing](http://blog.kevtris.org/blogfiles/Nitty%20Gritty%20Gameboy%20VRAM%20Timing.txt) — Kevin Horton's preliminary logic-analyzer notes on video memory access timing.
- [The Comprehensive Game Boy PPU Documentation](https://github.com/mattcurrie/mealybug-tearoom-tests/blob/master/the-comprehensive-game-boy-ppu-documentation.md) — Matt Currie's notes based on test-ROM results and emulator implementation; useful for raster-register behavior, but not an independent hardware measurement.
- [GBEDG PPU guide](https://github.com/Ashiepaws/GBEDG/blob/master/ppu/index.md) — secondary overview of the DMG PPU pipeline.
- [SameBoy PPU implementation](https://github.com/LIJI32/SameBoy/blob/master/Core/display.c) — fetcher/FIFO implementation comparison, not a hardware specification.
- [SameBoy 1.0.4 CPU write-conflict handling](https://github.com/LIJI32/SameBoy/blob/c87d2565ea2eef1d342d0828c041ca71b5784aff/Core/sm83_cpu.c#L115-L306) — pinned register-specific timing policies, including DMG SCX; use for implementation comparison and distinguish them from hardware measurements. Match source to the version returned by `retro_get_system_info`, since an installed `.info` file may be stale.

### Audio and cartridges

- [Pan Docs: Audio Registers](https://gbdev.io/pandocs/Audio_Registers.html) and [Audio Details](https://gbdev.io/pandocs/Audio_details.html) — channel sequencing, wave RAM and output behavior.
- [Pan Docs: MBCs](https://gbdev.io/pandocs/MBCs.html), [MBC2](https://gbdev.io/pandocs/MBC2.html), and [The Cartridge Header](https://gbdev.io/pandocs/The_Cartridge_Header.html) — mapper control and cartridge size fields.
- [SameBoy APU implementation](https://github.com/LIJI32/SameBoy/blob/master/Core/apu.c) — implementation comparison for channel timing and noise behavior.

## Test ROMs and reference implementations

- [Mooneye Test Suite](https://github.com/Gekkio/mooneye-test-suite) — hardware acceptance ROMs; its README documents real-hardware verification and model-specific results.
- [Game Boy test ROM collection](https://github.com/c-sp/game-boy-test-roms) — includes the v7.0 DMG screenshot reference used for Mooneye's manual `sprite_priority` check.
- [Mealybug Tearoom PPU tests](https://github.com/mattcurrie/mealybug-tearoom-tests) — DMG screenshot tests for register changes during Mode 3 (MIT).
- [Blargg DMG sound tests](https://github.com/retrio/gb-test-roms/tree/master/dmg_sound) — CPU-visible APU and CH3 wave RAM checks.
- [Blargg test ROM archive](https://gbdev.gg8.se/files/roms/blargg-gb-tests/) — downloadable test packs, including `dmg_sound.zip`.
- [Blargg DMG sound test sources](https://github.com/crzysdrs/blarggs-test-roms/tree/master/dmg_sound) — assembly sources and expected checksums.
- [SameSuite APU tests](https://github.com/LIJI32/SameSuite/tree/master/apu) — sample-accurate tests; most use CGB-only PCM registers and do not apply to this DMG core.
- [MBC3 Bank Tester](https://github.com/EricKirschenmann/MBC3-Tester-gb) — mapper bank diagnostic; its screenshot result does not cover frontend save-file persistence ([test procedure](https://github.com/c-sp/game-boy-test-roms/blob/master/src/howto/mbc3-tester.md)).
- [mGBA](https://github.com/mgba-emu/mgba) — independent libretro core for cross-core gameplay and frame diagnostics; output is not hardware ground truth.

## Gameplay and audio examples

- [GB Pong Clone](https://github.com/AlexMGiroux/gb-pong-clone) — 32 KiB ROM-only homebrew used for core input acceptance (MIT).
- [Big2Small](https://mdsteele.itch.io/big2small) — freely available MBC5 game used for local gameplay and save-persistence checks.
- [Tobu Tobu Girl](https://tangramgames.itch.io/tobutobugirl) — freely downloadable DMG homebrew; source is MIT and game assets are CC BY 4.0. Its [source and build settings](https://github.com/SimonLarsen/tobutobugirl) declare MBC1 + RAM + battery.
- [Canyon Racer](https://bbbbbr.itch.io/canyon-racer) — homebrew with mid-scanline SCY changes for parallax effects.
- [Game Boy Noise Test](https://github.com/not-chciken/gb-noise-test) — interactive CH4 settings and presets with DMG-01 reference recordings (Apache-2.0).

## Emulator development and build references

- [GBIT](https://github.com/jiayeli-hualien/gbit) — CPU instruction comparison used by this project.
- [Building a Game Boy emulator](https://wwssllabcd.github.io/2021/02/08/how-to-build-a-gameboy-emulator-1/)
- [Game Boy emulator notes (HackMD)](https://hackmd.io/@sysprog/BJvOWzFqv)
- [Game Boy emulator notes by RinHizakura](https://hackmd.io/@RinHizakura/BJ6HoW29v)
- [The Ultimate Game Boy Talk](https://media.ccc.de/v/33c3-8029-the_ultimate_game_boy_talk)
- [Thomas Spurden's Game Boy notes](https://thomas.spurden.name/gameboy)
- [Calling C++ from C](http://www.huoschen.idv.tw/blog/programming/c-calling-c++-functions/)
- [Creating a shared library with Make](https://stackoverflow.com/questions/8096015/creating-a-simple-makefile-to-build-a-shared-library)
- [GNU Make notes](https://jasonblog.github.io/note/gunmake/gunmake.html)
- [Makefile example](https://gist.github.com/wenchy/64db1636845a3da0c4c7)

The [architecture guide](architecture.md) and [DMG validation matrix](dmg-validation.md)
link these references near the relevant implementation and evidence.
