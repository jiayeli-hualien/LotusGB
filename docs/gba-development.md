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
  interleaved 48 kHz stereo audio.
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
- `WAITCNT` at `0x04000204` supports the Game Pak RAM wait selector, the three
  ROM wait-state regions' first and sequential access selectors, and PHI/prefetch
  register readback. CPU-step accesses to Game Pak ROM/EEPROM and Game Pak RAM
  add the selected wait clocks plus one normal transfer clock; 32-bit ROM
  accesses use two 16-bit transfers. The prefetch bit is stored but its buffer
  behavior is deferred. Tests cover register masks/reset, exact first and
  sequential ROM transfer totals, all three ROM regions, 32-bit splitting,
  Game Pak RAM timing, and loop progress under faster/slower settings.
- Basic audio reuses the four-channel DMG/CGB PSG implementation for GBA square,
  wave and noise channels. Direct Sound A/B use signed 8-bit FIFOs, timer
  overflow sampling, stereo routing and DMA1/2 special-timing refills. The core
  mixes these into 48 kHz stereo PCM. The mixed PSG/FIFO output is sampled at
  the SOUNDBIAS-selected cadence (32.768–262.144 kHz), quantized to the
  corresponding 9–6-bit PWM amplitude resolution, then resampled for libretro;
  SOUNDBIAS does not change the PSG oscillator clocks. The core applies its
  bias and 10-bit clipping. Source cadences above 48 kHz pass through an
  18 kHz fourth-order low-pass before linear resampling to the fixed libretro
  rate
  ([register
  behavior](https://mgba-emu.github.io/gbatek/#4000088h---soundbias---sound-pwm-control),
  [mGBA reference](https://github.com/mgba-emu/mgba/blob/master/src/gba/audio.c)).
  `make test` runs the MIT-licensed
  [GBA Audio Check ROMs](../tests/roms/gba-audio-check/README.md): a
  deterministic 440 Hz PSG tone and timer/DMA-driven Direct Sound A/B streams
  with different alternating left/right samples. These cover clipping,
  cadence and amplitude-resolution changes, PSG panning, a measured 420–460 Hz
  tone range, a measured 1950–2150 Hz timer/DMA Direct Sound signal, Direct
  Sound routing, the
  50%/100% Direct Sound volume ratio, and libretro sample delivery. GBA Sound 3
  also models the two 16-byte wave RAM banks, inactive-bank CPU access, 32/64
  sample playback dimensions, and the forced 75% volume setting; the same
  licensed homebrew test path covers both bank dimensions and volume behavior.
  That baseline also checks that an 8.192 kHz PSG tone passes while a
  32.768 kHz tone is attenuated before conversion to 48 kHz. A private Emerald
  Route 102 capture had identical left and right PCM because the
  game's Mono option wrote `SOUNDCNT_H=0x3302`, routing both Direct Sound
  FIFOs to both speakers.
  Upstream initializes new saves with Mono and uses `0x210E` for Stereo
  ([new-game defaults](https://github.com/pret/pokeemerald/blob/master/src/new_game.c),
  [M4A setup](https://github.com/pret/pokeemerald/blob/master/src/m4a.c)). A
  private controller-driven probe loaded the Route 102 save, selected Stereo
  in the game's settings, and resumed the same map. In the final five-second
  PCM window, Stereo left/right channels differed (difference RMS 349,
  correlation 0.941); the Mono capture's final five seconds matched exactly.
  This confirms that the game's Mono/Stereo choice changes libretro channel
  routing. It does not establish sound fidelity; listening against hardware
  remains unverified.
- DMA supports immediate, VBlank and HBlank transfers, including repeat and
  address-control modes, plus the four-word fixed-destination transfers used
  to refill Direct Sound FIFOs.
- ARM coverage includes the instruction groups exercised by the licensed ARM
  suite: conditions, branches, shifts and data processing; selected CPSR/SPSR
  behavior; multiply; word, byte and halfword transfers; SWP; and block
  transfers. The HLE BIOS implements the Div SWI used to report failures.
- Thumb mode passes the external suite's logical, shift, arithmetic, branch
  and memory groups, including stack operations, multiple transfers and
  interworking. This is test-suite coverage, not proof of every Thumb edge case.
- Each supported instruction has one prototype base cycle. WAITCNT-selected
  Game Pak bus waits are added to CPU steps, but internal CPU cycles, exact
  sequential-pipeline behavior and video/HBlank DMA transfer timing remain
  approximate.

## Known limits

This is a testable GBA prototype, not a general compatibility claim. The
external ARM and Thumb suites pass, but untested instruction edge cases remain;
unsupported instructions stop with an explicit CPU fault. Each supported
instruction still has one prototype base cycle; WAITCNT-selected Game Pak bus
waits are added, while internal CPU cycles, exact pipeline behavior, and
video/HBlank DMA transfer timing remain approximate. BIOS loading checks for a
readable 16 KiB file but does not yet verify
its recognized checksum. Without an external BIOS, implemented BIOS calls are
HLE-substituted.
With a BIOS, SWIs and IRQs enter their BIOS vectors, though unsupported CPU
instructions can still stop execution. Sound resampling filter quality,
analog mixing and exact sound timing remain approximations; state
serialization is unimplemented. Cartridge
battery saves are detected from the ROM's `SRAM_V`,
`FLASH_V`/`FLASH512_V`, `FLASH1M_V`, or `EEPROM_V` marker. The core supports
32 KiB SRAM, 64 KiB Flash, 128 KiB banked Flash, and 512-byte or
8-KiB EEPROM; EEPROM transactions use DMA and expand to 8 KiB when the game
uses the longer address form. GBA tests cover read/write and reset/reload
retention for these save types, plus the libretro save-memory interface. The
`SIIRTC_V001` GPIO RTC supports its serial register protocol and persists its
wall-clock base through the libretro RTC memory block. Synthetic RTC tests are
present; the private Emerald probe also confirmed that the clock advances
across a save/reload. Flash operations complete immediately;
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
`tests/include/gba_boot_rom.hpp` uses the guest CPU and BIOS `CpuSet` service
to draw a 4bpp Mode 0 tile background and a 16×16 sprite. Both the direct
system regression and the libretro callback check now compare every pixel in
the 240×160 frame; the callback check runs in XRGB8888 and 0RGB1555. The
separately licensed Pong ROM provides a gameplay and frontend callback
baseline in Mode 3, but these checkpoints do not establish broad GBA
compatibility or manual RetroArch acceptance. The callback also verifies
internal BGR555 channel ordering and 5-bit-to-8-bit expansion for an asymmetric
mid-range color. Private commercial-ROM probes are local diagnostics; keep
their ROMs and image captures out of the repository and CI.

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

On 2026-11-09, a private probe continued Emerald through the opening and rival
battle into Route 102, saved in-game, closed the runner, and reloaded the game
save. Continue showed the saved Route 102 location and 0:30 playtime, and the
overworld resumed there; the save bytes were unchanged by the reload. The RTC
sidecar advanced by 2 hours, 47 minutes, 38 seconds during the first closed
interval and by 16 minutes after a later reload. This meets the selected RTC
acceptance condition: the clock advances across save/reload. Berry growth or
other timed gameplay events are not required for this checkpoint. ROM, BIOS,
saves, captures and probe executables remained outside the repository.

A fresh-source interactive libretro check loaded the 128-KiB game-save and RTC
blocks into a new core session. It reached the title, showed the saved 0:30
playtime on Continue, restored an outdoor map scene, and moved the player in
response to right input. The Flash image remained byte-identical after the
check. The RTC block initialized, and its persisted time base advanced by
31,446 seconds from an input sidecar whose host timestamp was about 31,516
seconds old, leaving the effective clock continuous. No CPU fault was logged.
This is a headless current-source resume/input check, not a RetroArch frontend
acceptance. The harness, ROM, BIOS, saves, sidecars and captures remained
outside the repository.

After adding the SOUNDBIAS PWM resolution and stronger downsampling filter, a
separate 300-frame startup audio smoke delivered 241,100 stereo PCM frames
with left/right RMS levels of 2502.5/2519.4 and a peak of 19,908. Its final
capture was still at the copyright screen, so this confirms PCM output without
digital clipping but does not validate gameplay audio or sound fidelity.

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
   This is boot acceptance only, not acceptance as a playable game.
2. **Emerald single-player playability — bounded checkpoint passed:** private
   gameplay reached Route 102 after the opening and rival battle, then the
   game's own save survived closing and reopening the runner; Continue restored
   the saved location and playtime. This is a playable early-game checkpoint,
   not a claim that the full story or every game feature has been verified.
   Treat unintended emulator regressions as blockers for the recommended game.
   When behavior looks like a bug, compare it with reliable hardware
   documentation, targeted tests, or real hardware; reproduce an odd behavior
   only when the GBA itself exhibits it.
3. **Graphics and audio fidelity:** the core already implements Mode 0/1
   tiled backgrounds, affine backgrounds, 4bpp/8bpp tiles, palettes, regular
   and affine objects, windows and color effects. Keep the original
   homebrew tile/sprite pixel test and Pong gameplay regression. The private
   Emerald opening, title menu, truck interior and exterior scenes exercise
   real tiled backgrounds and objects; directional input exits the truck.
   Route 102 movement is now confirmed; additional game scenes remain follow-up
   coverage. Add rendering behavior when a scene demonstrates a gap, including
   more accurate mid-scanline writes and PPU timing. The MIT GBA Audio Check
   ROM now gives PSG output and SOUNDBIAS a repeatable core baseline. A private
   Emerald probe also confirms that selecting Stereo separates the game's
   left/right PCM output, while Mono produces matching channels. Sound quality
   and comparison with actual hardware remain open; implementation changes
   should follow a reproduced difference in the intended game.
4. **Timing and game-specific hardware:** WAITCNT readback and basic CPU-side
   ROM/RAM waits are implemented. Add prefetch-buffer behavior, then improve
   CPU internal-cycle and DMA timing when needed to fix a demonstrated game or
   visual/audio discrepancy. Keep the existing VBlank, HBlank, VCount, timer
   and DMA behavior under focused tests. `SIIRTC_V001` GPIO RTC is implemented;
   the selected Emerald check passes because the clock advances across
   save/reload. Broader GPIO devices or higher precision are follow-up work
   only if a target game needs them.
5. **Connectivity — deferred:** Link Cable and RFU support are lower priority
   because the target use is single-player. Save-state serialization also stays
   outside the current playability gate.

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
  boot path. The private Emerald probe also reached Route 102, saved in-game,
  and resumed the saved location and playtime after closing and reopening the
  runner. This passes the bounded early-game playability check; it does not
  establish full-story or broad GBA compatibility.
