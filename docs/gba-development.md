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
- Startup can execute an external 16 KiB GBA BIOS from RetroArch's system
  directory (`gba_bios.bin` or `bios.bin`). When no BIOS is available, the
  core falls back to an HLE handoff to `0x08000000` in ARM state, with a stack
  in IWRAM. No proprietary BIOS is bundled.
- The `GBA BIOS startup` core option selects `Automatic`, `Use BIOS` or
  `Skip BIOS`. Skip BIOS starts at the cartridge entry in ARM/System mode,
  initializes the System, IRQ and Supervisor stacks, sets `POSTFLG`, and
  initializes the affine identity registers. It omits the BIOS boot animation.
- ROMs containing the `FLASH1M_V` save marker get a 128 KiB Flash 1M device at
  `0x0e000000`. It supports the Macronix MX29L010 ID, two 64 KiB banks,
  byte programming, 4 KiB sector erase, chip erase and the Flash bank command.
  The battery buffer is exposed through libretro save RAM and survives a core
  reset; the frontend can restore it after loading the game.
- The bus maps ROM, EWRAM, IWRAM, I/O, palette RAM, VRAM and OAM. The current
  video path renders Mode 0 text backgrounds, Mode 1 text backgrounds plus
  affine BG2, Mode 2 affine BG2/BG3, bitmap Modes 3–5 with BG2 affine
  transforms, and forced blank; Mode 4 palette index 0 is transparent. Text
  backgrounds support 4bpp/8bpp tiles,
  256×256 through 512×512 maps, scrolling, flips, palette banks, mosaic and
  layer priority. Objects support regular and affine sprites, all shape/size
  combinations, 4bpp/8bpp tiles, 1D/2D mapping, bitmap-mode tile numbering,
  mosaic, priority, clipping, per-scanline rendering budgets and OBJ windows.
  Each scanline has a 1210-cycle OBJ budget, reduced to 954 when H-Blank
  Interval Free is enabled. OAM inspection costs two cycles per entry; regular
  sprites add their width minus two cycles, and affine sprites add eight plus
  twice their rendered width. Thus, a drawable regular sprite costs its pixel
  width and an affine sprite costs 10 plus twice its rendered width. Sprites
  clipped at the left edge reduce the drawing cost: regular sprites save one
  cycle per two clipped pixels (rounded up), and affine sprites save one cycle
  per clipped pixel. Objects with no pixels on the current scanline still incur
  OAM inspection but no drawing cost, following the
  [GBA sprite timing reference](https://lokathor.github.io/gba-from-scratch/ex3.html).
  The clipping adjustment follows mGBA's
  [OAM cost calculation](https://github.com/mgba-emu/mgba/blob/master/src/gba/renderers/common.c).
  OAM order is preserved, including horizontally offscreen objects that
  intersect the line
  ([GBATEK](https://mgba-emu.github.io/gbatek/#maximum-number-of-sprites-per-line)).
  Transparent pixels from later OBJs can promote the BG priority of an earlier
  opaque OBJ pixel, including transparent pixels from OBJ-window sprites; this
  follows the hardware comparison documented in
  [mGBA issue 2809](https://github.com/mgba-emu/mgba/issues/2809).
  WIN0/WIN1 layer masks, wrapped horizontal and vertical window ranges, alpha,
  brightness-up and brightness-down effects are rendered. Video-memory bus
  width behavior also models replicated byte writes
  to BG VRAM/palette RAM and ignored byte writes to OBJ VRAM/OAM.
- Mid-scanline writes to video registers, palette RAM, VRAM and OAM render the
  pixels already passed before applying the new state to the rest of the line.
  The raster boundary uses four prototype cycles per pixel.
- CPU mode switching banks FIQ, IRQ, SVC, Abort and Undefined registers and
  saved status. HLE startup calls the cartridge IRQ handler at `0x03007FFC`
  directly; BIOS startup enters the ARM IRQ vector at `0x00000018` and executes
  the BIOS dispatcher. Data-processing exception returns restore SPSR in each
  CPU exception mode.
- VBlank, HBlank and VCount status and enabled IRQ requests, IE/IF/IME
  registers, keypad input and the `IntrWait`/`VBlankIntrWait`/`Halt` BIOS calls
  support scanline effects and Pong's frame loop.
  Timers 0–3 support the four prescalers, count-up cascade and overflow IRQ
  flags. HLE also implements `RegisterRamReset`, `CpuSet`, `CpuFastSet`, LZ77
  decompression to WRAM/VRAM, `BgAffineSet` and `ObjAffineSet`.
- DMA supports immediate, VBlank and HBlank transfers, including repeat and
  address-control modes. Sound FIFO special-timing DMA is not implemented.
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
unsupported instructions stop with an explicit CPU fault. Timing is still a
one-cycle-per-supported-instruction prototype and does not model hardware wait
states. BIOS loading checks for a readable 16 KiB file but does not yet verify
its recognized checksum. Without an external BIOS, implemented BIOS calls are
HLE-substituted.
With a BIOS, SWIs and IRQs enter their BIOS vectors, though unsupported CPU
instructions can still stop execution. Sound FIFO DMA and state serialization
remain unimplemented. Cartridge battery saves are detected from the ROM's
`SRAM_V`, `FLASH_V`/`FLASH512_V`, `FLASH1M_V`, or `EEPROM_V` marker. The core
supports 32 KiB SRAM, 64 KiB Flash, 128 KiB banked Flash, and 512-byte or
8-KiB EEPROM; EEPROM transactions use DMA and expand to 8 KiB when the game
uses the longer address form. GBA tests cover read/write and reset/reload
retention for these save types, plus the libretro save-memory interface. RTC
cartridge devices remain unimplemented. Flash operations complete immediately;
busy status and write timing are not modeled, and EEPROM busy timing is not
modeled. The
mid-scanline raster split uses prototype CPU cycles and does not model video
bus wait states or the PPU's exact fetch pipeline. OBJ attributes are staged
for the incoming scanline at H-Blank; H-Blank DMA writes to OAM therefore take
effect after that staged scanline. Other H-Blank DMA changes are visible on
following scanlines.

The graphics regression tests cover tiled backgrounds, Mode 1/2 affine BGs,
BG2/BG3 composition, bitmap modes, sprites (including all regular shape/size
combinations and flips, 4bpp/8bpp tile reads, affine scaling and 8bpp sampling,
1D/2D tile mapping, transparent 8bpp pixels, bitmap-mode tile indexing,
scanline cycle limits, left-edge cycle savings for regular and affine sprites,
H-Blank OAM staging,
transparent-OBJ priority promotion, and semi-transparent OBJ-to-background
blending without OBJ-to-OBJ blending),
Mode 4 index-zero transparency, mosaic, wrapped window ranges, mid-scanline
display-register writes and color effects. The display pipeline resolves
overlapping OBJ pixels to one OBJ layer before compositing backgrounds and
applying color effects, as described in
[GBATEK's color-effects notes](https://mgba-emu.github.io/gbatek/#lcd-io-color-special-effects).
A small original homebrew ROM scene generated by
`gba_video_test.cpp` uses the guest CPU and BIOS
`CpuSet` service to draw a 4bpp tile background and a 16×16 sprite; the test
checks every output pixel. The separately licensed Pong ROM provides a
gameplay and frontend callback baseline in Mode 3, but these checkpoints do
not establish broad GBA compatibility or manual RetroArch acceptance. Private
commercial-ROM probes are local diagnostics; keep their ROMs and image captures
out of the repository and CI.

In a private Japanese Pokémon Emerald probe, the local BIOS was 16 KiB and its
MD5 matched the commonly used official BIOS image. The BIOS logo appeared and
execution handed off to the ROM by about frame 300. Before Flash 1M support,
the display stayed disabled. After Flash identification was implemented, the
game enabled all four backgrounds by frame 480, rendered the Game Freak
opening and an animated outdoor scene, and reached the Japanese title menu by
about frame 2100. Start opened the new-game menu; holding A advanced into the
Professor Birch introduction, which continued through frame 4800. No CPU fault
occurred. A follow-up run against the current core completed 16,000 frames
with no CPU fault; its frame-14,000 capture still showed an interior dialogue
scene.

On 2026-11-08, a manual probe with the current core advanced the Japanese
opening, entered the moving truck, and used rightward input to leave it. The
game then displayed the exterior scene with the truck, nearby building,
player sprite, NPC sprite and dialogue. This confirms that the game accepts
directional movement at the truck exit and that its real tiled backgrounds
and sprites render in both the truck and exterior scenes. The probe stopped
at the exterior dialogue; it does not yet establish free overworld movement,
gameplay progression, or in-game save/reload. RetroArch was closed after the
probe.

A follow-up private probe drove the same BIOS and a temporary Emerald ROM copy
directly through `GbaSystem`, without relying on RetroArch input. Pulsed A input
advanced the opening into the truck; right plus A produced the black map
transition, and the exterior was visible by frame 6240. Holding right alone
after the exterior appeared triggered a nearby NPC dialogue by frame 6360.
The core ran through frame 6480 without a CPU fault. This confirms a real
directional response and the exterior tile/sprite scene, while leaving longer
overworld exploration and in-game save/reload unverified. The ROM, BIOS,
screenshots and probe executable remained outside the repository.

This result confirms the first startup blocker: Pokémon Emerald calls
`CheckForFlashMemory()` before entering its main loop and clears the main
callback when Flash detection fails. Its Flash library identifies a 1-megabit
(128 KiB) Flash chip, now emulated for ROMs with the `FLASH1M_V` marker. See
the upstream
[`AgbMain`](https://github.com/pret/pokeemerald/blob/731ad5bfd6e6f265508d0efcca0ba42f9dcf5881/src/main.c),
[`CheckForFlashMemory`](https://github.com/pret/pokeemerald/blob/731ad5bfd6e6f265508d0efcca0ba42f9dcf5881/src/load_save.c),
and [Flash implementation](https://github.com/pret/pokeemerald/blob/731ad5bfd6e6f265508d0efcca0ba42f9dcf5881/src/agb_flash_1m.c).
The cartridge library defines its 1-megabit Flash size as 131,072 bytes in
[`flash_internal.h`](https://github.com/pret/pokeemerald/blob/731ad5bfd6e6f265508d0efcca0ba42f9dcf5881/include/gba/flash_internal.h).

## Pokémon Emerald implementation gates

1. **Emerald boot — accepted; battery interface implemented:** Flash 1M
   identification, banking, erase/program commands and libretro battery
   storage pass synthetic command and persistence checks. The private Emerald
   probe gets past the former white screen, shows the opening scenes and enters
   the new-game introduction; the user accepts this as a successful game boot.
   In-game save/reload remains a separate playability check and is not required
   to claim that Emerald boots.
2. **Stable game loop:** improve CPU and GamePak timing beyond the current
   one-cycle-per-instruction model, starting with `WAITCNT` ROM wait states and
   sequential access/prefetch behavior. Keep the existing VBlank, HBlank,
   VCount, timer and DMA behavior under focused tests, then add a private probe
   that records progress through startup without saving game-derived captures.
   Emerald sets `WAITCNT` during startup and enables a VCount interrupt.
3. **Broaden actual-scene graphics coverage:** the core already implements Mode 0/1
   tiled backgrounds, affine backgrounds, 4bpp/8bpp tiles, palettes, regular
   and affine objects, windows and color effects. Keep the original
   homebrew tile/sprite pixel test and Pong gameplay regression. The private
   Emerald opening, title menu, truck interior and exterior scenes exercise
   real tiled backgrounds and objects; directional input exits the truck.
   Free overworld movement and additional game scenes remain follow-up
   coverage. Add rendering behavior when a scene demonstrates a gap, including
   more accurate mid-scanline writes and PPU timing.
4. **Complete single-player experience:** add the sound hardware and FIFO DMA
   needed for game audio, and RTC/GPIO behavior for clock-based events. These
   are not the first blockers for reaching a visible scene. Link/RFU support
   and save-state serialization can remain outside this single-player gate.

Public game-level ROM checks must use clearly licensed homebrew. Use private
commercial ROMs only for local manual play checks; keep their ROMs, saves and
game-derived captures out of the repository and CI.

## BIOS startup acceptance

- **File read:** `Use BIOS` finds the selected file in RetroArch's system
  directory, reads it successfully, requires exactly 16 KiB and logs the
  loaded path. This establishes successful reading and expected file size.
- **BIOS identity:** for the commonly used official image, compare MD5 with
  `a860e8c0b6d573d191e4ec7db1b1e4f6`, which [Libretro lists for mGBA's
  optional GBA BIOS](https://docs.libretro.com/library/mgba/). LotusGBA does
  not enforce this fingerprint; identity is a separate manual check.
- **BIOS execution and handoff:** start at address zero in ARM Supervisor
  mode, execute the BIOS reset path without a CPU fault, and reach cartridge
  code at `0x08000000`. The synthetic BIOS fixture verifies mapping and
  handoff. In the private real-BIOS probe, the BIOS logo appeared and Emerald
  reached its ROM startup without a CPU fault.
- **BIOS bypass:** `Skip BIOS` ignores any BIOS file and starts directly at
  `0x08000000` in ARM state and System mode, with System, IRQ and Supervisor
  stacks, `POSTFLG`, and affine identity registers initialized. A guest-code
  probe verifies all three stack pointers; the startup test also checks the
  initial PC/mode, ARM state, `POSTFLG`, affine registers, cleared RAM and
  hidden default OAM entries. A libretro check confirms the cartridge runs
  even when a looping BIOS file is present. This path intentionally omits the
  BIOS logo and boot animation.
- **Option timing:** the core reads the BIOS mode when content loads. Restart
  the content after changing the option.
- **Game compatibility:** the BIOS logo and successful handoff validate the
  boot path. Game play has separate checkpoints: the private Emerald probe
  reaches the new-game introduction and still needs overworld and save/reload
  checks.
