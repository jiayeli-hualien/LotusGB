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
to draw a 4bpp Mode 0 tile background and 16×16 sprites. The sprites exercise
Y=128 and Y=159 as visible positions, Y=160 as offscreen, and Y=255 wrapping
to the top edge. Both the direct system regression and the libretro callback
compare every pixel in the 240×160 frame; the callback check runs in XRGB8888
and 0RGB1555. The separately licensed Pong ROM provides a gameplay and frontend callback
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
5. **Connectivity:** the [local two-player cable subsystem](#sio-and-rubysapphire-link)
   has original-ROM test coverage. Private Ruby/Sapphire acceptance, network
   gameplay acceptance, RFU and save states remain pending. The paired
   Netpacket implementation started and ran synchronized frames in two
   RetroArch processes on the same Mac. Cross-device testing remains open.

Public game-level ROM checks must use clearly licensed homebrew. Use private
commercial ROMs only for local manual play checks; keep their ROMs, saves and
game-derived captures out of the repository and CI.

## SIO and Ruby/Sapphire link

LotusGBA implements a local two-system 16-bit multiplayer SIO exchange
through the `gba_link_2p` libretro subsystem. Original ROMs exercise that
path. Commercial Ruby/Sapphire exchange and battle still need private
manual acceptance.

The first cable target is two-player 16-bit multiplayer SIO. The
[Ruby/Sapphire decompilation](https://github.com/pret/pokeruby/blob/master/src/link.c)
selects this mode at 115200 bps, uses SI/SD and player ID for role detection,
and reads the four receive slots in its handshake. Its ongoing exchange is
driven by serial completion and Timer 3. The
[FireRed instruction manual](https://www.videogamemanual.com/gba/Pokemon%20-%20FireRed%20Version%20%28USA%29.pdf)
directs players to the Game Link Cable for communication with Ruby/Sapphire.

| GBA SIO mode | Scope for Ruby/Sapphire |
| --- | --- |
| 16-bit multiplayer | Required for the two-cartridge cable path. The parent initiates, and all participants receive the same words in player order. |
| Normal 8-bit and 32-bit | Standalone internal-clock transfers complete with high input bits when no compatible peer is modeled. Two-endpoint transfers and external-clock reception are not implemented. Normal 32-bit is also used by the Wireless Adapter. |
| UART | Separate asynchronous serial mode. |
| GPIO and Joy Bus | RCNT selects GPIO or Joy Bus instead of SIOCNT's normal, multiplayer or UART modes. Device protocols such as GameCube connectivity are separate targets. |

The [GBATEK SIO register descriptions](https://mgba-emu.github.io/gbatek/#sio-multi-player-mode)
describe the hardware interface. LotusGBA models SIOCNT/RCNT mode selection,
parent start, ready and busy status, participant IDs, four receive slots, and
the per-system serial completion IRQ. The parent samples both send words at
start. The round takes 36 bit periods at the selected baud, accounting for the
start and stop bits on both words. Both endpoints receive the words together
at completion. Mode exit, reset or peer removal aborts an active round.
This model covers a two-device word exchange. Intermediate wire transitions,
hardware turnaround timing, extra devices, and network behavior remain open.
Standalone Normal 8-bit and 32-bit internal-clock transfers now run for the
selected bit count and clock rate, read high bits when no compatible Normal-mode
peer is modeled, clear the start bit, and request the serial IRQ when enabled.
External-clock transfers remain busy until a clock source is implemented.

Original ARM and Thumb tests cover readiness, parent-only start, all four
baud settings measured with Timer 3, serial IF handling, pair teardown,
and repeated exchange through the libretro callbacks. An original in-memory
multiplayer probe ROM now also runs on a local two-system `GbaSystem` pair,
performs two guest-driven transfers, and checks both received words and player
IDs in each endpoint's SRAM. The same probe ROM runs over queued remote SIO at
0, 100, and 200 ms simulated RTT; each run completes both guest-driven rounds
with the expected received words and no SIO error. This validates the probe's
SIO protocol path, not Emerald's own timeout behavior. The libretro tests
also cover independent controllers, side-by-side output in both pixel
formats, and independent battery memory across reset. A macOS RetroArch
1.22.2 launch with the original probes and audio disabled verified the
subsystem command and verified that both battery files were written and
reloaded independently. Synthetic remote-SIO checks cover readiness events,
sequenced host start and child reply, matching receive slots, IRQ behavior,
stable roles and ready lines across two consecutive rounds, independently
advanced endpoint clocks under synthetic delay, and rejection of an invalid
sequence. The remote-link test also mirrors Emerald's public
`CheckMasterOrSlave` predicate: only player ID 0 with SD high and SI low is
recognized as master; the child reports SI high and a nonzero ID. This verifies
the SIOCNT role bits, but not the game's separate `0x8FFF` handshake-state
progression. The core-level packet test exchanges two Netpacket rounds. These
checks do not establish commercial-game behavior or WAN latency tolerance.

### Network timing policy and current checkpoint

Network mode separates frontend presentation cadence from each guest's
emulated clock. Outside recent SIO activity, the two guest clocks may advance
independently. During a hot link period, the core bounds frame lead to two
frames; it does not lock whole frames, invent received words, or raise a fake
serial interrupt. A remote transfer start carries its guest-cycle timestamp so
packet arrival does not directly choose the guest-visible SIO start time. This
is an emulator-side compatibility policy for modern hosts and network links,
not a claim that a physical cable synchronizes frame clocks. It leaves the
game ROM unchanged and may alter how long guest code waits while an SIO word is
in transit.

The synthetic GBA core suite passes the independent-frame and bounded-link
checks, including real word completion and input retention while paced. A
private Japanese Emerald replay with zero added packet delay kept the guest
cycle totals within 261 cycles at frame 7,323, but both games exited the
expected multiplayer state instead of reaching the Cable Club flow. Therefore
clock separation and SIO mechanics are implemented and unit-tested, but this
change has not yet passed the target game's link handshake. Inspect the
game-visible handshake state before widening latency tolerance or relaxing SIO
semantics further. ROMs, saves and captures remain private.

The GBA core now registers `RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE` from
`retro_init()`, as the libretro API requires, rather than from
`retro_set_environment()`. The unit test checks that registration is absent
before `retro_init()` and present afterward. A private RetroArch 1.22.2 run on
the Ubuntu PC loaded the rebuilt core and logged `GBA Netplay packet transport
started as host`, confirming the frontend called the core's host start
callback. A same-PC CLI client connected at TCP level but did not log its
Netpacket start callback, and the host did not log a connected peer; this is
not a successful Netpacket exchange. The GUI join workflow remains the next
frontend integration check.

### Run a linked pair locally

Load LotusGBA in RetroArch, select **Game Boy Advance Link (2 Players)** under
**Subsystems**, then select the two ROMs and start the subsystem. With a
command-line build of RetroArch, the equivalent invocation is:

```sh
retroarch -L /path/to/lotusgba_libretro.so --subsystem=gba_link_2p first.gba second.gba
```

Player 1 is the cable parent and appears on the left. Player 2 appears on the
right and uses controller port 2. Audio comes from player 1. The core exposes
each cartridge's SRAM/Flash and RTC separately. The subsystem uses the
extensions `.srm` and `.rtc` for the first slot and `.srm.2` and `.rtc.2`
for the second. Use copies of existing saves and check the paths RetroArch logs
for the subsystem before starting a commercial-ROM test.

Private Ruby/Sapphire acceptance should cover Cable Club handshake, a trade
with both saves reopened, and a battle with matching game state. RFU and
other SIO modes can be evaluated separately when a target needs them.

#### Japanese Pokémon Emerald

Private two-player libretro sessions completed a Cable Club trade and battle
with both `Use BIOS` and `Skip BIOS`. The two saved parties contained the
expected traded Pokémon and passed checksum checks. Both battle replays
completed a forced send-out and showed the same winner and loser on both
screens. The results screens matched across BIOS modes. Those results cover
the local two-system subsystem and the earlier paired Netplay design; they do
not validate the current transfer-level remote-SIO path.

Before nested IRQ support, the HLE battle replay failed during the forced
send-out. One serial interrupt was still pending when the next transfer
began, and the receiving game flagged a checksum mismatch. The HLE now
allows IRQ nesting when cartridge code re-enables IRQs and restores each
interrupted context in order. An original test cartridge checks that a
serial IRQ runs inside a VBlank handler, reads both transferred words, and
then returns through both contexts.

#### Network acceptance target

Validate GBA Cable Club trade and battle at 100 ms and 200 ms round-trip
latency. Check that both saves contain the expected traded Pokémon after
reopening, and that both players see the same battle actions and outcome.
Measure input response separately. Run additional cases with latency
variation and packet loss once the fixed-latency cases pass.

Each device loads the same ROM as ordinary single-cartridge content in
RetroArch. Netplay checks the ROM fingerprint and save/RTC sizes, and shares
the host's RTC epoch. Each device keeps its own battery and RTC data; save
contents are not sent to the peer. The host models the parent endpoint and
the client models the child endpoint. Ready changes, parent transfer starts,
and child words travel as reliable SIO packets. Each device runs one GBA
instance and advances its own CPU, PPU and APU between link events.

For cross-device testing, use the same LotusGBA build and the same ROM
revision on both devices. Load the ROM as ordinary content with separate
copies of the two saves. Verify each player's save loads in RetroArch
before hosting or joining via Netplay. The session reboots each local
cartridge from its own battery save using the shared RTC epoch. Enter the
Cable Club after the session starts. When testing a trade, save in-game,
quit RetroArch normally, then reopen each player's save and inspect the party.

The 2026-11-18 PC Cable Club attempt is not acceptance evidence for the current
source: its RetroArch log loaded the test-directory `lotusgba_libretro.so`, a
GCC 15.2.0 x86_64 ELF, and that PC build tree's SIO and libretro source hashes
differ from the current Mac worktree. On 2026-11-19, `make build-gba-zig-pair`
built both OS cores on the Mac with Zig 0.17.0. The transferred Linux core's
hash matches the Mac build and its runtime dependencies resolve on Ubuntu;
however, RetroArch has not yet loaded this new pair, so current-source
cross-device gameplay remains unverified. Use the exact pair and steps in the
private test SOP before interpreting another connection result.

The remote-SIO core test runs a single parent GBA, exchanges two sequenced
SIO words with a fake remote child, verifies the receive slots and SRAM
result, and confirms video callbacks continue while no peer input frames are
sent. The `GbaSystem` tests separately cover child start handling, readiness,
transfer timing and completion IRQs. These checks cover the new packet path
and local SIO state machine, not cross-process timing or commercial-game
compatibility. The shared RTC epoch uses UTC and advances with emulated
cycles. An RTC test covers a midnight rollover, calendar writes, and
persistence bytes under Taipei and New York host time zones.

On 2026-11-19, a bounded in-memory headless run of the private Japanese Emerald
ROM using HLE reached the title Continue menu. With the remote-link harness,
both endpoints reported multiplayer SIO ready during the opening; selecting
Continue briefly entered Normal 32-bit SIO and then returned to multiplayer
ready. The harness observed no multiplayer transfer starts or replies during
that run. A one-endpoint Continue input displayed the saved Pokémon Center,
but this does not establish paired game progress. This probe confirms ROM
startup and SIO mode transitions only; it does not validate a Cable Club
handshake or the remote packet path with this game.

A temporary SIOCNT write trace showed the Normal 32-bit mode being started and
then reset before the game returned to multiplayer mode. This resembles the
RFU ID probe `AgbRFU_checkID` in the public
[Pokémon Emerald decompilation](https://github.com/pret/pokeemerald/blob/master/src/librfu_sio32id.c),
which uses Normal 32-bit SIO and a fixed adapter-identification exchange. The
trace does not map the observed Japanese-ROM PCs to that exact function, so
this is a likely RFU detection path, not proof that the cable protocol needs
Normal 32-bit transfers. The next game-level target remains a 16-bit
multiplayer transfer after entering Cable Club.

Two macOS RetroArch processes with an original probe ROM booted paired
emulation and each reported completing 60 synchronized frames. The local
TCP relay added 100 ms each way and RetroArch reported 216–225 ms ping.
In a second loopback run, each frontend started with a different marker
in its own 32 KiB save. With 50 ms added each way, the probe wrote both
received SIO words to SRAM. After normal frontend exit, both save files
contained the received words and retained their respective markers.
A headless TCP probe with ARM64 and translated x86-64 macOS cores
also verified both SIO rounds and both local save markers with an added
200 ms RTT. The corresponding mixed-architecture RetroArch attempt
joined the lobby but timed out during startup. The core now refreshes
the startup deadline when it sends or receives valid startup data.
A retest reached paired startup on both sides, but did not capture
the 60-frame milestone. The host reported 3,509 ms ping in that
run. The mixed-architecture probes used the same Mac.
In separate single-Mac replays with three and six frames of delay per
direction (about 100 and 200 ms RTT), Japanese Emerald completed a trade.
The latest party save section and each Pokémon's data passed checksum
verification on both sides for both delays. Each team contained the
expected received Pokémon and kept the other party member. The outputs
matched byte-for-byte between the two delay cases. Four standalone
reloads reopened the party menu and showed the received Pokémon in
each player's party.

Separate Japanese Emerald battle replays completed at three and six
frames of delay per direction (about 100 and 200 ms RTT). Both sides
continued after a forced send-out for each player. Both final result
screens matched the local HLE two-cartridge control pixel-for-pixel
in both cases. All of these commercial-game replays ran on a single
Mac. Cross-device acceptance remains open.

Profiling the local linked Emerald pair on the Mac found repeated internal
timer and IRQ register reads going through the guest bus. Reading the
same stored values directly reduced a 720-frame HLE boot replay from
22.61 s to 10.22 s on that machine. The final frame and both save images
matched byte-for-byte. This benchmark excludes RetroArch and the network.
With this change, two native macOS RetroArch processes again reached the
60-frame milestone with a probe ROM and 200 ms of added RTT. RetroArch
reported 210–217 ms ping. This run ended before confirming save writeback.

An earlier SIO-word Netpacket prototype exchanged data with original probe
ROMs, but headless Japanese Emerald showed a communication error after the
host confirmed entry, even without injected delay. A local paired scheduler
reached the room-entry prompt with the same saves and inputs. The current
remote-SIO implementation uses `GbaSystem`'s multiplayer readiness, start,
word, busy and IRQ state machine instead of the old per-frame paired-input
transport. Its next acceptance checkpoint is Japanese Emerald cable-room
entry, followed by one trade and save reload. If the room-entry failure
repeats, inspect the game's role/readiness handshake before trying latency
workarounds or fabricating link signals.

On 2026-11-19, a private two-system harness loaded the Japanese Emerald ROM
and separate saved games into two `GbaSystem` instances. After selecting the
Direct Corner Trade Center, both games entered the multiplayer SIO wait
state (`SIOCNT` `0x600B` and `0x601F`) and displayed a wait message. A queued
remote-SIO run with three frames of delay in each direction (about 100 ms
RTT) delivered 239 complete start/reply pairs and had one further start in
flight; the games remained at the wait prompt without a communication error.
A local `runLinkedFrame` control also reached the wait prompt. The initial
game flow briefly wrote `SIOCNT` `0x5081` (normal 32-bit mode with transfer
start set) before switching to multiplayer mode. Replaying the 1,245-frame
startup macro with the updated core showed `0x5081` on both systems at frame
1,125, then multiplayer-ready values `0x2008` and `0x201C` at frame 1,245;
no multiplayer transfer had started by then. This does not establish whether
the game depends on the Normal-mode completion result. Neither run reached the
trade room, so the evidence is limited to mode transitions and delayed SIO
survival, not Cable Club entry or a trade pass. The next investigation is the
game's role/readiness handshake and the first multiplayer transfer after
confirming the Cable Club prompt.

An exact replay of the same Japanese Emerald input sequence on 2026-11-19
reached the two-player communication prompt in both the local linked scheduler
and the queued remote-SIO harness. The local pair remained in multiplayer mode
after the confirmation inputs. The remote run exchanged 359 start/reply pairs,
then the game left multiplayer mode and displayed its communication error.
There was no configured packet delay, but this was not zero emulated-time
latency: the harness delivered packets only after both full-frame runs. A
second private harness that delivered between endpoint frame runs still
reproduced the error. Direct synchronous callbacks also proved unsuitable:
the child rejected a later start while its previous transfer had not advanced
through its SIO bit time. The remaining mismatch is sub-frame event ordering
and independent endpoint progress, not evidence of a missing BIOS routine.
Matching the local cycle scheduler requires timestamped link events or a
transfer-boundary pacing scheme; frame-boundary packet polling alone is not
enough for this game's handshake. A follow-up change pauses the host's guest
clock at a pending remote transfer boundary. Replaying through frame 5,026
then avoided the earlier communication-error screen: the host stayed at the
"start communication" prompt and the client stayed in its communication
wait. This is progress but not Cable Club entry; the endpoints are still at
different game prompts, and the new pacing has not yet been tested at 100 or
200 ms RTT. The host displays repeated frames while waiting for its SIO reply.
The same bounded replay with three-frame one-way packet delay (about 100 ms
RTT) ended at frame 5,026 with 111 start/reply pairs and both endpoints still
in multiplayer mode. With six-frame one-way delay (about 200 ms RTT), it ended
at that checkpoint with 60 pairs and one reply in flight; the host was waiting
on SIO and the client remained ready. These are headless queued-packet probes,
not separate-device network runs, and neither reached the Cable Club room.
The input path now latches button presses made while the host guest clock is
stalled and exposes them until an emulated frame completes; the unit test
covers a press and release during the stall. A further 200 ms headless replay
retained the multiplayer session through frame 5,268, with 79 starts and 78
replies delivered and the host still waiting on SIO. The confirmation input
was latched, but the guest had not advanced far enough to verify that the
Cable Club prompt accepted it. This replay therefore confirms input retention
and link survival at the checkpoint, not progress into the room.

The remote-SIO protocol carries host emulated-cycle watermarks and timestamps
transfer starts. A Japanese Emerald replay exposed a boundary case where the
child reached the host watermark with one SIO cycle remaining. The child now
gets 64 cycles of instruction-boundary slack to finish that transfer; the GBA
unit and libretro tests cover delayed starts, cycle caps, packet delivery, and
the HLE startup path. `make -C src/gba_libretro test build_dir=../../build`
passes.

Private headless replays of the same Japanese Emerald save pair completed
repeated SIO start/reply exchanges at simulated 0, 100, and 200 ms RTT. At
frame 3,121, the 100 ms run had completed 61 pairs and the 200 ms run 38 pairs,
with no communication error. After 3,000 more idle frames, the 200 ms run had
completed 252 pairs through frame 6,121. The games remained in their Cable
Club waiting screens at those checkpoints. The zero-delay run reached the
host's “two players connected” prompt after 1,214 pairs. After the two
A-button confirmations, the local linked baseline entered the room, but the
remote zero-delay replay returned to its communication-start prompt. Thus the
transport survives the simulated delays, while remote Cable Club entry,
trade/reload, and battle are still unverified. These are headless queued-packet
tests, not separate-device network runs; ROMs, saves, screenshots, and logs
remain outside the repository.

On 2026-11-19, the remote protocol moved to v4 to stop comparing the peers'
absolute emulated-cycle counters. The host no longer sends cycle watermarks or
timestamps with SIO starts; a received start begins against the child's local
clock. Synthetic tests pass with the child advancing several frames ahead
before receiving a start. Private HLE replays of the same Emerald save pair
then completed 67 start events and 66 replies at simulated 100 ms RTT, and 38
starts and 38 replies at 200 ms RTT through frame 3,121, without a communication
error. The host was still in a transfer and both games remained at the Cable
Club wait state. At zero simulated delay, the child ran about 40 seconds of
emulated time ahead of the host by frame 7,323, and the two-player room prompt
still did not advance. Stopping the child immediately after each reply was
also tried and rejected: the game must execute code after transfer completion
to consume the result. Thus independent clocks remove cross-device cycle
comparison, but do not yet provide game-level synchronization or room entry.
These remain private queued-packet probes rather than separate-device network
tests.

A follow-up experiment let the host CPU keep running after its local SIO bit
time elapsed, while leaving SIOCNT busy until the remote word arrived. At zero
configured packet delay this kept parent and child cycle totals within 81
cycles through frame 6,121 and completed 3,529 SIO starts/replies. However,
after the scripted communication confirmation, Emerald displayed its
communication-error screen by frame 7,323. In this harness, “zero delay” still
means delivery at a frame boundary. The error is consistent with a game-side
timeout, but this run did not isolate which handshake condition failed.
This experiment was reverted: the current host again pauses at the SIO
transfer boundary. The child-PC trace below was initially treated as a guest
link-wait loop, but later memory-write tracing showed that interpretation was
wrong: with `r2 = 0x03002360`, the loop reads `r2 + 28` at IWRAM
`0x0300237C`. The public Emerald `Main` layout places `intrCheck` at offset
`0x1C`, and `WaitForVBlank` clears then polls its VBlank bit 0. The loop is the
game's normal frame wait, not evidence of a link timeout. The relevant
game-level signal is the link lag state and its effect on Cable Club progress,
which needs to be inspected directly before choosing a finer-grained
scheduling policy.

The public Emerald [`LinkVSync`](https://github.com/pret/pokeemerald/blob/master/src/link.c#L1990-L2034)
implementation narrows down the timing constraint: it has the master start a
transfer from its VBlank callback during handshake/connected states, while the
non-master counts VBlanks without a serial interrupt and marks lag after more
than ten. This makes direct round-trip-per-transfer forwarding sensitive to
RTT: at 200 ms RTT, a response cadence can span more than ten 60 Hz VBlanks
even though each endpoint advances its CPU on its own clock. The exact
Japanese-ROM path and the effect of this lag marker on room entry should still
be confirmed; the marker is not itself proof of a disconnect.

A separate child-side heuristic paused after the same guest PC repeated 32
times within 4,096 cycles. It caught the normal VBlank wait at ROM address
`0x080008CC`: the code polls `gMain.intrCheck` at IWRAM `0x0300237C` for bit 0,
which the VBlank interrupt sets. This matches the public Emerald
[`Main` layout](https://github.com/pret/pokeemerald/blob/master/include/main.h)
and [`WaitForVBlank`](https://github.com/pret/pokeemerald/blob/master/src/main.c).
From frame 3,121 to 7,323 the heuristic-held child advanced only about 24
million cycles while the host advanced about 587 million, despite continuing
to exchange SIO words. That heuristic was reverted. It did not identify the
game's SIO wait condition; a scheduling pause must be based on an actual
link-state signal, not a repeated-PC test.

The next investigation should capture the guest link status and Cable Club
handshake state around a stalled room entry, then compare direct event
forwarding with a locally replicated cable pair using delayed/synchronized
inputs. Any latency-tolerant model must preserve the guest's data and error
semantics; do not suppress the guest's lag/error indication without proving
that the corresponding serial data and peer state are valid.

Comparing the private Japanese-ROM event log with the public Emerald
[`DoHandshake`](https://github.com/pret/pokeemerald/blob/master/src/link.c)
implementation narrows this checkpoint further. The game sends `0x8FFF` when
its master-side handshake is armed and `0xB9A0` otherwise; it counts valid
player slots, then requires more than one player, a stable count across
handshake rounds, and `0x8FFF` in receive slot 0 before declaring the link
handshake complete. In the captured replay, the first parent transfer sent
zero, then the next 353 starts sent `0xB9A0`; none sent `0x8FFF`. That run
therefore did not reach the player-data block or its `GameFreak inc.`
signature check, and cannot explain the later command-stream checksum
failure. The next diagnostic should find
why the master never arms its handshake marker—specifically whether the game
sees master ID/SD status and advances its handshake state—before changing
checksum handling. This is evidence from one private trace interpreted
against the public decompilation, not proof that every Japanese-ROM path is
identical.

A clean rerun of the scripted Japanese Emerald HLE replay at six frames of
packet delay in each direction (about 200 ms RTT) completed through harness
frame 7,323 with 338 SIO starts and 338 child replies and no communication-error
screen. Both endpoints still showed their Cable Club communication-wait
messages. The child had advanced 2,057,009,620 cycles while the host had
advanced 822,946,905, confirming substantial guest-clock drift. This shows
that the VBlank lag marker is a protocol signal to investigate, not proof of
an immediate disconnect; the current demonstrated failure remains lack of
room-entry progress. This is a private frame-queued replay, not a two-device
network run.

A controlled zero-delay comparison then used the same two Japanese Emerald
saves and the same 48-segment input script
(`/private/tmp/emerald-v3-start-communication.txt`) with the current paired
scheduler and current remote-SIO host/child packet ordering. The local pair
stayed cycle-aligned and, after the final A input, the host sent `0x8FFF` at
cycle 1,889,222,874. Both systems completed that transfer with receive slots
`0x8FFF, 0xB9A0` and serial IRQ IF changing from `0x0000` to `0x0080`. The
remote host and child exposed the same role-status register values (`SIOCNT`
`0x600B`/`0x601F`, `RCNT` `0x000B`/`0x000F`), and completed SIO transfers
also set serial IRQ on both endpoints. The remote host never sent `0x8FFF`;
it was still sending `0xB9A0` after the same final input, while the child
remained at the communication-wait screen. Neither run reached player-data
transfer or checksum validation.

The first reproducible divergence is guest-clock progress, not a bad SIO word
or missing completion IRQ. At harness frame 6,722, the local pair had advanced
1,888,182,913 cycles on both systems. The remote child had advanced
1,888,190,672 cycles, but the remote host only 1,308,138,165 cycles, after
2,065 starts/replies. The 580,052,507-cycle gap is almost exactly 2,065 GBA
frames. The first transfer also had matching `0x0000` words and IRQs in both
modes: the local pair completed both endpoints at cycle 728,284,947, while
remote mode completed the child at its local cycle 728,090,730 and the host at
728,284,948. The remote completion clocks are independent; both receive slots
were correct and both set serial IF. At frame 7,323, the host/child totals
were 1,392,603,989 / 2,057,009,812 after 2,366 starts/replies. The local pair
reached 2,057,001,409 cycles on both systems and emitted the `0x8FFF` marker. In
`GbaSystem::runFrame`, the host returns early when local SIO bit time expires
while the peer word is pending (`src/gba/gba_system.cpp`, `runFrame`). With
frame-boundary packet polling, this spends about one host guest frame per
completed remote transfer while the child continues to advance. The later A
input therefore reaches a host still thousands of guest frames behind. Direct
inspection of the Japanese ROM's `gLink` confirms the first protocol-phase
divergence. The ROM SHA-1 matches the Japanese Emerald disassembly's
`rom_jp.sha1`. Its `DoHandshake` and `CheckMasterOrSlave` assembly use
`0x03003210` as the link-state base; field offsets agree with the public
[`Link` layout](https://github.com/pret/pokeemerald/blob/master/include/link.h#L195-L219).
At checkpoint 46 (frame 6,723, after the same final A input), local and remote
both still have state 2 (`HANDSHAKE`), correct parent/child roles and IDs,
player count 2, `0xB9A0` in the handshake slots, and no error or checksum flag.
The sampled `serialIntrCounter` already differs: local 35/35 versus remote
17/18. After the next 600 input-free frames (checkpoint 47, frame 7,323), the
local host and child reach state 4 (`CONN_ESTABLISHED`), with `0x8FFF` in
handshake slot 0 and checksum field `0x961E`; remote host and child remain in
state 2, still exchange `0xB9A0`, and retain zero hardware, checksum, queue or
lag errors. During those 600 outer frames, the local host and child each
advance 168,537,598 cycles; the remote child advances 168,538,242 cycles, but
the remote host advances only 84,268,795 cycles while 300 additional SIO
rounds complete. Thus the earlier counter mismatch is progress drift, while
the first sampled handshake-phase mismatch is the host not having advanced
far enough to arm/send `0x8FFF`; this is not a bad checksum or wrong
master/slave role. The Japanese ROM's [`DoHandshake`](https://github.com/pret/pokeemerald-jp/blob/master/asm/link.s#L4908-L5037)
and [`LinkVSync`](https://github.com/pret/pokeemerald-jp/blob/master/asm/link.s#L4757-L4820)
disassembly show that marker is selected by guest handshake state.

A fresh replay on 2026-11-19 confirmed the earlier point of divergence. At
checkpoint 42 (frame 3,121), the local pair had advanced 876,676,417 / 876,676,419
cycles; remote mode had advanced 802,441,493 / 876,680,135. The game's
`serialIntrCounter` was 17 / 17 locally and 8 / 9 remotely, while both modes
still showed the same master/child IDs, player count, `HANDSHAKE` state,
`0xB9A0` handshake words, and clear hardware/checksum error flags. This is the
earliest divergent checkpoint in the 48 sampled input segments, not a claim
that the clocks first separate at that exact frame.

Reproduction used the same ROM and save files in both modes, with the 48-line
input script `/private/tmp/emerald-v3-start-communication.txt`. The temporary
trace executable is `/private/tmp/emerald-correct-link-trace`; its source is
`/private/tmp/emerald_correct_link_harness.cpp` plus
`/private/tmp/gba_system_trace.cpp`. Run it with `0` for remote-SIO mode or
`0 local` for the local-pair control, feeding the same input file to stdin.
The fresh replay invocations were:

```sh
/private/tmp/emerald-correct-link-trace 0 < /private/tmp/emerald-v3-start-communication.txt > /private/tmp/emerald-start-remote-recheck.stdout 2> /private/tmp/emerald-start-remote-recheck.stderr
/private/tmp/emerald-correct-link-trace 0 local < /private/tmp/emerald-v3-start-communication.txt > /private/tmp/emerald-start-local-recheck.stdout 2> /private/tmp/emerald-start-local-recheck.stderr
```

The replay logs are `/private/tmp/emerald-start-remote-recheck.stdout` and
`/private/tmp/emerald-start-local-recheck.stdout`; SIO traces and IWRAM
snapshots are under
`/private/tmp/emerald-correct-v4-0/` and
`/private/tmp/emerald-correct-v4-0-local/`. The executable reads the private
Japanese ROM and both saves directly; all logs and snapshots remain outside
the repository. The minimal scheduler change to evaluate is a bounded
guest-frame credit for the remote child: keep both endpoints within one guest
frame/VBlank epoch while letting each local CPU advance independently between
cable events. When one endpoint stalls at a pending SIO word, prevent the
other from running hundreds of frames ahead; resume it as the delayed word
completes. Keep the existing SIO word, completion IRQ and error behavior
unchanged. This is a recommendation, not a validated fix. A prior variant
that let the host CPU run through a pending word avoided clock drift but later
displayed the game's communication error. Test a frame-credit scheduler first
with an original two-endpoint probe that checks both VBlank counters, SIO
receive slots, and IRQs, then replay this exact Japanese Emerald input/save
pair before trying higher RTT.

### Wireless Adapter as a high-latency path (research)

The RFU route is worth evaluating because Emerald's Wireless Adapter path
uses packet slots, per-player send/receive queues, and sequence IDs rather than
the cable path's direct 16-bit transfer handshake. The game dispatches
`RfuVSync` from VBlank, and the public Emerald RFU setup allows up to four
master frames but disables link-loss recovery; its non-initialization failure
counter is configured to 300. These details suggest a more natural place to
buffer network packets, but do not establish that the game tolerates Internet
latency. The library still runs on the guest's frame/interrupt schedule and
has its own failure handling. See the public
[`link_rfu_2.c`](https://github.com/pret/pokeemerald/blob/master/src/link_rfu_2.c#L2370-L2393),
[`main.c`](https://github.com/pret/pokeemerald/blob/master/src/main.c#L320-L349),
and [`librfu.h`](https://github.com/pret/pokeemerald/blob/master/include/librfu.h).
LotusGBA does not implement RFU yet; an RFU network prototype would need its
own packet/slot timing model and 100/200 ms Emerald trade/battle checks. Treat
it as a promising alternative to test, not an assumed latency fix.

### What other emulators do about link latency (research)

The strongest directly relevant precedent found is VBA-M's wired GBA link
implementation. Its source documents the same failure pattern seen in our
Emerald traces: the process doing blocking socket/semaphore waits falls behind,
while the peer can accumulate VBlanks without Serial IRQs; Pokémon can declare
the link dead after ten quiet VBlanks. VBA-M paces transfer starts against the
peer's emulated clock and briefly throttles a linked endpoint when it leads the
recent transfer stream by more than a small frame window (three frames for the
socket path, two for IPC). The tighter pacing is active only while the link is
hot, so players can reach the Cable Club at different times. This is a useful
design precedent for bounded frame credit, but the source does not establish a
measured 100 or 200 ms WAN guarantee. See VBA-M's
[link implementation](https://github.com/visualboyadvance-m/visualboyadvance-m/blob/master/src/core/gba/gbaLink.cpp#L3464-L3512).

mGBA's desktop multiplayer coordinator instead coordinates local emulator
instances through SIO lockstep. Its wait/signal and cycle accounting advance
linked local systems together; this is not evidence of an upstream Internet
cable transport or high-RTT tolerance. See its
[multiplayer coordinator](https://github.com/mgba-emu/mgba/blob/master/src/platform/qt/MultiplayerController.cpp#L1311-L1464).

RetroArch's standard Netplay synchronizes controller inputs and deterministic
game state; its FAQ says this mode is not GBA link-cable emulation. The FAQ
lists gpSP's Wireless Adapter emulation as a separate way to trade Pokémon.
That confirms RFU as an existing game-facing alternative, but does not claim
that it tolerates a particular RTT. See the
[Netplay FAQ](https://github.com/libretro/docs/blob/master/docs/guides/netplay-faq.md#can-i-play-gb-gbc-gba-psp-3ds-games-with-multiple-people-via-retroarch-netplay).

The practical direction is to keep wired SIO as the broad-compatibility path
and solve its clock skew with bounded guest-time progress, preserving transfer
words, interrupt timing and game-visible errors. Evaluate RFU as a second,
Emerald-friendly protocol path with its own queue and slot timing. Neither
approach should claim 200 ms support until the same game checkpoints pass with
that simulated RTT; increasing a wall-clock receive timeout alone cannot
prevent guest VBlank and IRQ counters from diverging.

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
