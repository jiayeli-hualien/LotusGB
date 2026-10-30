# DMG implementation and validation matrix

This is the canonical status matrix for the current DMG target. “Implemented” means the behavior exists in LotusGB; it does not imply complete hardware accuracy. The table separates implementation, test evidence and known gaps. Dated investigation notes below preserve earlier failures and fixes; use the row summaries for current status.

The latest expanded PPU timing result (2026-10-25): all 14 cases pass, including
`hblank_ly_scx_timing-GS` and the sprite Mode 0 timing ROM. Earlier result
counts below are historical checkpoints.

The current system can either execute an optional external DMG boot ROM or start from a fast synthetic post-boot profile. CGB behavior and unsupported cartridge hardware are outside this matrix.

## Target model and PPU acceptance

LotusGB currently targets the DMG software-visible behavior. Labels such as
`DMG`, `CPU CGB C`, and `CPU CGB D` identify different console or hardware
revisions; they are not separate software API versions. Game Boy models share
a register-driven display interface, with CGB adding extensions, but that
does not promise identical pixels for every mid-scanline register race. Exact
output from deliberately timed Mode 3 writes can differ across revisions.

Use this acceptance boundary:

- Treat DMG-labelled Mooneye cases, DMG Acid2, focused DMG unit tests, and
  common-game behavior as the current acceptance evidence.
- Model variation does not make DMG behavior irrelevant: DMG observations are
  still in scope for this DMG target. CGB-only output and emulator screenshots
  without a matching hardware result cannot decide DMG correctness.
- Do not use an image from another hardware revision, or an emulator output
  without hardware provenance, as a pass/fail criterion; keep it as an
  investigation note only.
- Reopen a model-specific raster discrepancy when a matching hardware capture
  or independently hardware-validated expectation is available. Record the
  hardware model with that evidence.

For emulator validation, classify this as model-dependent or insufficiently
documented hardware behavior, not C++ undefined behavior. It should not block
the DMG target unless a supported game or a DMG-backed test demonstrates an
observable problem.

### Synchronization available to games

The PPU draws pixels progressively through internal fetch and FIFO state. Games
can synchronize through `LY`, `STAT` mode status, `LY=LYC`, and the related
interrupts; VBlank and HBlank are common times to update display state. Mode 3
length varies with scrolling, the window, and objects, so a fixed delay is not
a dependable substitute for checking PPU status. The documented software
interface does not expose a command to flush the pixel pipeline into a
cross-model canonical state. Turning off the LCD/PPU is a disruptive full
display stop, and is only safe during VBlank; it is not a per-scanline flush.

Raster effects can still be valid game behavior. Their writes need to be
synchronized to the intended PPU event, and some sampling details are
revision-dependent—for example, the documented SCY bitplane sampling differs
before and from CGB-D onward. For shared DMG-compatible output, games generally
use stable mode/scanline boundaries rather than relying on a particular
mid-Mode 3 race. See [Pan Docs rendering](https://gbdev.io/pandocs/Rendering.html),
[scrolling](https://gbdev.io/pandocs/Scrolling.html), and
[LCD control](https://gbdev.io/pandocs/LCDC.html).

| Area | Implemented behavior and notable DMG details | Current validation | Known gaps / next useful check |
| --- | --- | --- | --- |
| CPU instructions and timing | CPU advances in M-cycles and reports T-cycles. Implemented behavior includes arithmetic, ALU, loads/stores, CB operations, control flow, stack and interrupt sequences; 11 base opcodes remain explicitly unsupported and fail through the libretro error path. | `make test`: `cpu_instruction_test.cpp` and `cpu_cycle_test.cpp` check externally visible registers, flags, bus order and cycle phases across register, immediate, memory, stack, jump and interrupt forms. `ADD SP,e8` is checked over all 65,536 SP-low-byte/offset combinations. The pinned GBIT integration comparison is separate. | CPU phase tests follow GB-CTR and check observable state; they are not physical-DMG timing measurements. The 11 unsupported opcodes and DMG OAM corruption from IDU accesses remain outside implementation; other internal timing is not fully hardware-validated. |
| Timer and interrupts | DIV/TIMA/TMA/TAC edge-based counting, delayed TIMA overflow reload and its four-T-cycle write window, DIV-write edges, IF/IE, interrupt priority and CPU interrupt entry. | `make test`: `timer_test.cpp` verifies TAC frequencies and confirms TIMA overflow requests Timer IF on reload; `game_boy_system_test.cpp` checks that the timer IRQ wakes HALT and reaches its handler; `cpu_instruction_test.cpp` covers CPU interrupt entry. | All 13 Mooneye `acceptance/timer` ROMs pass, including TIMA/TMA writes during reload. The PPU group includes interrupt-entry and HALT timing ROMs, and the interrupt group adds ten EI/DI, RETI, HALT and IF/IE checks. This does not cover every interrupt race. |
| Address bus and cartridge | Address mapping and WRAM echo; ROM-only, MBC1, MBC2, MBC3, and MBC5 banking. Special cases include MBC2's 512×4-bit RAM and MBC3 RTC latch, HALT, and day rollover. | `make test`: `address_bus_test.cpp`; 27 Mooneye standard MBC1/MBC2/MBC5 cases pass. Libretro integration runs CPU code from MBC1 bank 2, writes battery RAM, unloads the game, restores the SRAM block, and verifies banked code reads and updates it. Other libretro tests cover save RAM/RTC memory-block restoration across reset and reload. | RetroArch `.srm` disk write/reload was manually verified with Big2Small on 2026-10-23. RetroArch `.rtc` disk write/reload was verified on 2026-10-26 using a temporary MBC3 timer-only probe: clean exit wrote the 24-byte RTC block, and a second RetroArch process restored and advanced it. These verify one cartridge/frontend path each, not every mapper or game; RTC evidence and limits are recorded under `build/test-roms/mbc3-rtc-persistence/`. Tobu Tobu Girl remains an MBC1 gameplay target; high-score persistence requires completing a level and is not a required manual acceptance step. MBC30-sized RAM and other mapper families are unsupported; MBC5 rumble banking works, but motor output is not exposed. |
| Save states | Version 1 DMG snapshots include CPU instruction progress, hardware pipelines, RAM, mapper/RTC state, and frame/audio state. Libretro uses a fixed 512 KiB buffer and validates before atomic commit. | `save_state_test.cpp` and libretro GB Pong regressions are part of `make test`. Core tests compare 30 subsequent video and PCM callbacks after fresh-load restore in both pixel formats; invalid states leave the next frame running. `make build`, `make test`, and the pinned GBIT comparison passed. On 2026-10-30, macOS RetroArch 1.22.2 passed GB Pong save/advance/restore, further input, and restoration from the same file after a frontend relaunch; see [session identity and evidence](save-states.md#macos-retroarch-acceptance-2026-10-30). The user reported Steam Deck same-session and full-relaunch restore/control passing on 2026-10-31, with no current tearing; see [Deck evidence and identity limits](steam-deck-check.md#m5-save-state-checkpoint--user-reported-pass-2026-10-31). | Installed Deck hash and frontend/OS versions were not supplied. Running RTCs retain wall-clock behavior. Future-version migration and rewind/run-ahead performance are untested. |
| Serial, unconnected DMG | SB/SC registers, unused SC bits high, free-running 8192 Hz internal bit clock, MSB-first shifting with incoming ones, eight-bit completion, serial IF request, abort and externally clocked waiting. Serial clock is independent of DIV and continues in HALT. Startup profiles restore model-specific clock phase. | `make test`: `dmg_serial_test.cpp` checks register, bit/completion, and boot/post-boot divider-phase boundaries; system tests check serial IRQ/HALT/RETI and continued completion despite repeated DIV resets. Mooneye `boot_sclk_align-dmgABCmgb` passes with both fast startup and the local standard DMG BIOS. | The Mooneye result is for its documented DMG ABC/MGB target, not all DMG revisions or physical hardware. No real peer, external clock input, accessories or CGB fast clock are implemented. Component/system tests do not establish full hardware serial conformance. |
| Joypad | Active-low button lines, direction/button selection, falling-edge interrupt requests, and DMG STOP wake on a selected P10–P13 line falling low. STOP wake is separate from IE/IME interrupt acceptance. | `make test`: `joypad_test.cpp`, `game_boy_system_test.cpp`; `testSelectedJoypadLineWakesStopWithoutInterruptAcceptance` checks unselected input keeps STOP active, selected A input wakes it, IF is requested, execution resumes and no interrupt vector is entered with IME=0 / IE=0. Libretro input regression uses GB Pong. | STOP signal behavior follows [Pan Docs](https://gbdev.io/pandocs/Reducing_Power_Consumption.html#using-the-stop-instruction); no physical-DMG result is available. Exact wake phase, already-low-at-entry behavior and unusual STOP-entry races remain unverified. The libretro input/callback phase can shift VBlank-synchronized visible response by one callback; this is demonstrated with a synthetic ROM, not hardware-validated. See the [input-phase report](#libretro-input-phase-and-vblank-response-2026-10-26). |
| PPU rendering | 160×144 DMG shade framebuffer with background, window and objects, tile addressing, palettes, transparency and DMG priority. BG and OBJ use separate 16-pixel FIFOs; selected LCDC, palette and tile-map changes are sampled progressively. OBJ descriptors are selected at scanline entry and fetch stalls remain coarse. | `make test`: `dmg_ppu_test.cpp` covers pixel output and selected register changes; DMG Acid2 passes its exact framebuffer regression. The selected sprite-priority image matches its reference. Four optional GBMicrotest latch captures are visual diagnostics only; see [GBMicrotest](#gbmicrotest-ppu-latch-diagnostics). | Mode 3 pixel timing and shared BG/OBJ bus arbitration lack model-matched hardware validation. Canyon Racer repeatability and cross-core image comparisons are documented in the [homebrew guide](homebrew-check.md); they are not hardware oracles. See the [PPU investigation](ppu-scrolling.md). |
| PPU modes and Mode 3 timing | 456-dot scanlines; VBlank/STAT requests; final-line LY reset and delayed LYC comparison; mode-based VRAM/OAM access. Mode 3 starts at 172 dots, adds `SCX & 7`, a six-dot visible-window penalty, and modeled 6–11-dot object-fetch stalls. Normal-line readable LY changes at dot 452; LCD startup retains its dot-454 edge. | `make test` covers selected mode/access boundaries and CPU writes during blocked modes. Eleven selected Mooneye PPU/interrupt ROMs pass; the expanded 14-case PPU set passed 14/14 in its latest recorded run. | These tests do not establish full raster accuracy. SCX/SCY fetch sampling and BG/OBJ arbitration remain simplified or unverified against matching DMG hardware. The [PPU guide](ppu-scrolling.md) records the model and scoped diagnostics. |
| OAM DMA | FF46 schedules 160 bytes at one byte per M-cycle after startup, supports delayed restart, and arbitrates CPU OAM/source-bus access; DMG DMA sources `$E0`–`$FF` mirror `$C0`–`$DF`, independently of CPU OAM/I/O mappings. | `make test`: `dmg_oam_dma_test.cpp`; six Mooneye OAM DMA cases pass: `basic`, `reg_read`, `sources-GS`, `oam_dma_start`, `oam_dma_timing`, `oam_dma_restart`. | Basic read contention is modeled; exact write-induced bus corruption, PPU/DMA rendering contention, and sub-M-cycle bus effects remain unmodeled. |
| APU and audio output | DMG pulse 1/2, wave 3, and noise 4; stereo routing and PCM output. Special CH3 behavior includes delayed sample fetch, fetch-cycle wave RAM access, and retrigger corruption. | `make test`: `dmg_apu_test.cpp`, timer/system integration tests, and libretro callback tests. Optional `make test-roms` runs [Blargg DMG sound tests](https://github.com/retrio/gb-test-roms/tree/master/dmg_sound) 09, 10, and 12; all three passed in the 2026-10-26 run against the current core. | The selected ROMs focus on CH3 wave RAM behavior, not full APU conformance. CH4 waveform quality still benefits from listening or comparison against DMG reference recordings. |
| Startup, frame stepping, and libretro | The default fast path starts at `$0100` with a synthetic DMG post-boot state calibrated to the BIOS handoff. When `dmg_boot.bin` (or the compatible fallback name `gb_bios.bin`) is present in RetroArch's system directory, the core maps its 256 bytes at `$0000-$00FF`, starts the CPU at `$0000`, and unmaps it when the boot program writes nonzero to `$FF50`. Reset repeats the selected startup path. `stepFrame()` advances a nominal 70,224 T-cycles. | `make test`: system and libretro tests cover the synthetic BIOS overlay, `$FF50` handoff, optional system-directory loading, malformed-image diagnostics, content errors, STOP warnings, and callback-exception boundaries. With the local standard DMG BIOS, Mooneye `boot_regs-dmgABC`, `boot_hwio-dmgABCmgb`, and `boot_div-dmgABCmgb` all pass. The no-BIOS fast profile also passes all three after matching the handoff CPU/I/O values and divider phase. The core .so remains a local build artifact. | The fast profile skips the logo animation and initializes tested handoff state; it is not a byte-for-byte snapshot of all CPU-visible and internal memory (including logo-derived VRAM and OAM contents). Logo rendering/header checks and model-specific power-on behavior are not independently verified. Reserved-opcode diagnostics remain `stderr`-only, and placeholder handling continues execution. See [startup profile and boot-ROM scope](#startup-profile-and-boot-rom-scope). |

The Mooneye `manual-only/sprite_priority` DMG frame also matches its v7.0
reference image exactly (0/23,040 pixels differ). This verifies sprite-priority
composition for that scene, not per-dot OBJ fetch timing; see the
[reproducible visual check](ppu-scrolling.md#mooneye-manual-sprite-priority-image-2026-10-25).

## Startup profile and boot-ROM scope

LotusGB supports two startup paths. With no external image, the default system
bypasses the boot ROM and starts a supported DMG cartridge at `$0100` with a
synthesized post-boot state. With an optional 256-byte DMG image in RetroArch's
system directory, the core executes it from `$0000`; writing nonzero to
`$FF50` permanently exposes the cartridge ROM again. The preferred filename
is `dmg_boot.bin`, matching the core's firmware metadata. `gb_bios.bin` is
also accepted. Missing firmware falls back to the fast profile. A present but
unreadable or wrong-sized image reports a load error instead of silently
falling back.

The implementation was exercised with the user's local standard DMG image
(`~/roms/gb/gb_bios.bin`, 256 bytes); the BIOS itself remains outside the
repository. With BIOS execution, Mooneye `acceptance/boot_regs-dmgABC`,
`acceptance/boot_hwio-dmgABCmgb`, and `acceptance/boot_div-dmgABCmgb` all
pass. The no-BIOS fast path also passes all three after its post-boot timer
counter was set to `$ABC8`, matching the actual handoff counter captured at
PC `$0100` from each of these three ROMs. Startup initialization sets P1 to
`$CF`; NR13 and NR23 return `$FF`, as expected for write-only registers.
Pan Docs lists the DMG/MGB handoff values at PC `$0100`, including P1 `$CF`
and DIV `$AB`, and identifies the Mooneye acceptance tests as evidence
([Power-Up Sequence](https://gbdev.io/pandocs/Power_Up_Sequence.html)).

This validates the BIOS handoff CPU registers, tested hardware-register
values, and divider phase. The no-BIOS profile skips the logo animation and
does not draw the Nintendo logo. It synthesizes this tested handoff state;
it does not restore every internal memory byte produced by the animation,
such as logo-derived VRAM and OAM contents. Those differences remain outside
the fast-profile acceptance boundary unless a game exposes an observable
problem.

Captured at PC `$0100` with the three Mooneye ROMs:

```text
A=$01 F=$B0 B=$00 C=$13 D=$00 E=$D8 H=$01 L=$4D SP=$FFFE
Internal divider counter=$ABC8 (DIV reads $AB)
```

`F` follows the cartridge header checksum. The fast profile initializes these
CPU values and the tested I/O/divider state; it begins directly at the game
entry point, without the boot animation.

The focused local Mooneye run is reproducible with
`build/mooneye_rom_runner <rom.gb> --boot-rom ~/roms/gb/gb_bios.bin`.

Earlier, before the fast-profile divider phase was corrected, a local Mooneye
diagnostic run with the synthetic profile passed `acceptance/boot_regs-dmgABC`
but failed `acceptance/boot_hwio-dmgABCmgb` and `acceptance/boot_div-dmgABCmgb`.
The first hardware-I/O mismatch was at `$FF04` (DIV): expected `$AD`, got
`$AC`. The updated fast profile and optional boot-ROM path later passed all
three tests, as described above. No commercial ROM or derived capture was used
in this diagnostic. Test sources: [boot_hwio-dmgABCmgb](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/boot_hwio-dmgABCmgb.s)
and [boot_div-dmgABCmgb](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/boot_div-dmgABCmgb.s).

## DMG logo animation cross-core comparison (2026-10-27)

The user reported that the upper and lower parts of the Nintendo logo moved at
different times in macOS RetroArch; the effect was not observed with SameBoy.
The cause identified in LotusGB was submission of a partially updated PPU
framebuffer. `stepFrame()` advances 70,224 T-cycles per callback, but the BIOS
enables the LCD partway through a callback. A diagnostic trace with this BIOS
found subsequent callbacks approximately 14,428 PPU cycles after frame start,
about 31.6 scanlines into the visible frame. The working framebuffer is drawn
incrementally, so it could contain lines from two PPU frames at submission.

LotusGB now copies the working framebuffer at VBlank and submits that completed
snapshot through libretro. The PPU regression in `dmg_ppu_test.cpp` verifies
that the snapshot stays stable while the next frame is drawn and updates at
VBlank. CPU frame stepping and audio timing are unchanged. `make test` and
`make build` passed after the change.

For a direct comparison, LotusGB and SameBoy's installed macOS ARM64 libretro
core ran the same `boot_regs-dmgABC` homebrew ROM with the same locally supplied
DMG BIOS for 340 callbacks. Before the fix, 291 of 340 callback images matched
exactly; with the completed-frame snapshot, 339 of 340 matched. The remaining
difference at zero-based callback 334 matched SameBoy's callback 335 and was a
whole logo transition one callback earlier. The earlier claim that all 340
callbacks matched before the fix was incorrect and is withdrawn. These
captures used a libretro runner, not the RetroArch window or physical hardware.

After the revised core was installed, the user reported that visible tearing
had improved on Mac. At the time of the initial retest, the core in RetroArch's
core directory was still the older build; the actual file selected for that
retest was not established. macOS builds now produce an up-to-date `.dylib`, and
`make install-retroarch-core-macos` explicitly updates the installed core; see
[macOS development setup](macos-development.md#graphical-testing).

Both LotusGB and SameBoy report about 59.7275 FPS to RetroArch. Display pacing
and any speed adjustment or repeated frames depend on the frontend and monitor;
this change addresses incomplete framebuffer submission. Neither the cross-core
comparison nor the user's visual report establishes exact physical-DMG timing
or rules out all other display artifacts.

### Frame coherence at the libretro callback boundary (2026-10-29)

`testLibretroPublishesCoherentFramesAcrossLcdStartupPhases` now checks the
actual video callback in `libretro_core_test.cpp`. An original synthetic ROM
changes the whole background palette only during VBlank. Every submitted
image must therefore be uniformly black or white and must keep alternating.
Five LCD-enable delays deliberately shift PPU scanning relative to frontend
callbacks. The test exercises both XRGB8888 and the default 0RGB1555 fallback,
including the byte pitch and one video callback per `retro_run`.

All 400 callbacks passed on the native macOS build (40 per delay and format),
and `make test` passed. A negative control compiled a temporary copy of the
adapter that submitted the working framebuffer instead of the completed
snapshot. The new test rejected it at delay 1, XRGB8888 callback 1, pixel
`(0, 10)`. Production core code was not changed for that control. Logs and its
diagnostic JSON are under `build/test-roms/frame-coherence/`.

This establishes sensitivity to the earlier partial-frame submission bug and
coverage of the current adapter for this synthetic scene. It does not rule out
game-specific PPU rendering errors or identify the cause of the Deck report.
The user subsequently identified the scene below, then reported no tearing in
a later Deck check or Mac testing. The Deck core was not rebuilt by this
test-only change. The report is closed for now without a confirmed core defect.

### Yellow intro lightning investigation (2026-10-29)

**Conclusion:** the reported scene is Pokémon Yellow's opening Pikachu
lightning animation. The user reports that the tear did not recur in a later
Deck check and that Mac testing shows no tearing. The cause of the original
intermittent Deck report remains unverified; no core defect was confirmed.
Completed-frame submission prevents the earlier partially rendered frame bug,
but does not establish correct palette-write timing or synchronized physical
display presentation.

In the public [Yellow disassembly](https://github.com/pret/pokeyellow/blob/master/engine/movie/intro_yellow.asm#L565-L647),
intro scene 14 changes the background and object palettes using a repeating
sequence; scene 15 toggles palette bits every four timer steps. This makes
palette update timing a concrete investigation target, while establishing
that flashing itself is intentional in that reference version. It does not
prove that the user's reported split is an intended effect.

The reference intro loop runs its scene update and sprite animation before
calling `DelayFrame`; the [delay routine](https://github.com/pret/pokeyellow/blob/master/home/delay.asm)
waits for the next VBlank interrupt. The palette animation is therefore
updated by the main loop between VBlank waits, not by a per-scanline HBlank
routine. In LotusGB, the PPU emits pixels dot by dot and `renderPixel` applies
the current BGP/OBP values; a CPU palette write can consequently change later
pixels in the same frame. This is capable of representing a mid-frame palette
transition. It does not by itself establish whether its scanline timing matches
the original Game Boy, especially because the user's ROM hash differs from the
reference disassembly's listed retail build.

The user's local ROM was found outside the repository and its header identifies
Pokémon Yellow. Its SHA-1 does not match the retail UE ROM documented by the
disassembly, so the source is a behavioral reference, not a verified exact
match. The Deck and Mac observations are non-reproductions, not an A/B settings
result or proof that the fault belongs to either frontend. No production core
code or Deck binary changed during this investigation. The report is closed for
now; reopen if it becomes reproducible. Keep commercial-game validation private and manual,
with ROMs, saves and captures outside the repository and CI. Any eventual
automated regression should use an original synthetic reproducer of the
identified hardware behavior. No production PPU change is justified yet.

## Libretro input phase and VBlank response (2026-10-26)

**Conclusion:** LotusGB and SameBoy apply a scripted frontend input at different
PPU phases. A controlled VBlank-only ROM showed that this can move the visible
response by one video callback. This is a real adapter timing difference, not a
measured 1 ms wait in LotusGB and not evidence that either phase matches real
hardware. Keep the current frame stepping until a target-game or
hardware-backed result supports a change.

### Hypothesis and setup

LotusGB's `retro_run()` polls input, advances `GameBoySystem::stepFrame()`, then
renders ([adapter](../src/libretro/libretro_core.cpp#L337),
[frame step](../src/lib/game_boy_system.cpp#L79)). The default post-boot PPU
starts at `LY=0`, and `stepFrame()` advances 70,224 T-cycles, one nominal DMG
frame. SameBoy 1.0.4 polls input before running to its next VBlank boundary
([libretro adapter](https://github.com/LIJI32/SameBoy/blob/v1.0.4/libretro/libretro.c#L1351-L1428),
[frame loop](https://github.com/LIJI32/SameBoy/blob/v1.0.4/Core/gb.c#L1164-L1185)).
Libretro requires input polling at least once per `retro_run()` and one video
callback per run, but does not prescribe the PPU phase for either callback
([core development guide](https://docs.libretro.com/development/cores/developing-cores/)).

The hypothesis was that these callback boundaries differ by the final ten
VBlank scanlines: 4,560 dots, or about 1.087 ms at the nominal DMG clock. A
game that only reads input in its VBlank handler could therefore act on the
same frontend input in different callbacks. Real hardware exposes the
currently selected active-low button lines through JOYP; its interrupt is
triggered by a selected line's falling edge, not by a fixed 1 ms sampling timer
([Pan Docs: Joypad Input](https://gbdev.io/pandocs/Joypad_Input.html),
[Joypad interrupt](https://gbdev.io/pandocs/Interrupt_Sources.html#int-60-joypad-interrupt)).

### Controlled ROM results

Two temporary probes were derived from the repository's MIT-licensed GB Pong
ROM header and logo. Both cores ran in fresh working directories through
`build/libretro_rom_runner`; input intervals use zero-based frame indices with
an exclusive end.

1. **First JOYP read:** after 160 neutral runs, press A for input interval
   `160 161 A`. The ROM records `LY` on its first `$FF00` read that sees A
   pressed. LotusGB records `LY=0`; SameBoy records `LY=144`.
2. **VBlank-only response:** after 200 neutral runs, press A for `200 201 A`.
   The ROM reads `$FF00` only in its VBlank ISR and changes the background
   palette when A is pressed. At `LY=144`, both cores record the ISR input.
   SameBoy's callback after input run 200 already shows the palette change;
   LotusGB's callback after run 200 is still unchanged and its callback after
   run 201 shows the change. The output therefore differs by one callback.

The SameBoy image retains 746 pixels from boot-logo tile data; those pixels do
not change the timing result. The exact cores and probe ROMs are pinned by
SHA-256 below. RGB, PNG, SRAM, input scripts and ROMs are retained locally at
`~/gb_emu/test-results/lotus-gbemu-frame-phase-diagnostic/`; generated ROMs and
captures are not committed.

| Artifact | SHA-256 |
| --- | --- |
| LotusGB core | `b24d849c4a8b64d995c3ec860f2d37602d0ce04bca80190c6c43ccb9fbbc30dd` |
| SameBoy 1.0.4 core | `5f3cbc610d1de2c04d41319b6678bece8c5bc78b1067436e1f6d9d440b89b56e` |
| First-read probe ROM | `e0242007d5c71b3eafd0dfc3c7f14864f673a481c9e7716a0aa68d073d8205e4` |
| VBlank-response probe ROM | `d0275c85606ba12a21ece2c0e96f2e0c0b86d76452675388c250d92d379843ca` |
| Base GB Pong ROM | `9c5e337db0ee1416f2c7f1de02e7e0a78a6064cf1da49209b4bc1a36a7b35415` |

### What to validate with games

Big2Small remains a suitable routine gameplay check for startup, ordinary
control response, stable frame delivery and save persistence. Its successful
run does not close the specific VBlank-boundary question. The synthetic probe
already covers that mechanism, so do not add artificial boundary requirements
to Big2Small or repeatedly retest the same case through unrelated games.
Validate F-1 Race separately if that title is available: this probe does not
show whether it samples input or updates scrolling in VBlank. Until a
target-game or hardware-backed case points to a preferred phase, treat the
measured difference as known and leave production timing unchanged.

## Test workflow

The canonical required gate, targeted Mooneye groups, licensed homebrew
profiles, and visual diagnostics are listed in the
[testing workflow](testing.md). Keep ROM suites outside the repository unless
the ROM's own terms permit redistribution. This matrix records what each test
establishes and its limits; the testing guide records when and how to run it.

## GBMicrotest PPU latch diagnostics

The optional local workflow pins and captures `800-ppu-latch-scx`,
`801-ppu-latch-scy`, `802-ppu-latch-tileselect`, and
`803-ppu-latch-bgdisplay`. Their source comments describe the selected
mid-scanline write delays. The upstream author says the suite was checked on
DMG-CPU-08 hardware, though the exact revision is uncertain; see the
[GBMicrotest source](https://github.com/aappleby/gbmicrotest).

On 2026-10-26, all four ROMs ran for four frames and delivered four valid video
callbacks each. The output frames showed their test patterns rather than a
solid black frame. The contact sheet and individual captures are under
`build/test-roms/gbmicrotest-ppu-latch/`. These are useful for inspecting
whether a PPU change affects the same latch probes. They are not pixel-perfect
expected images, do not expose an automatic pass/fail signal for these visual
cases, and do not replace model-matched hardware comparison.

## Validation rule

Keep this matrix scoped to implemented behavior and the active DMG target. When a feature changes, update its row and add the narrowest useful regression. Add external ROMs when they provide evidence that a unit test cannot, rather than trying to pass every available test suite before continuing game development.

## Timing-change workflow

For a timing issue, use this sequence before changing production behavior:

1. Record the baseline for the failing case and nearby cases that already pass.
2. Write down the hardware expectation and its source. Treat a ROM's tested
   behavior as evidence for that case, not as proof that every nearby timing
   rule works the same way.
3. Trace the smallest reproducer on one clock: CPU bus access, peripheral
   event, visible register value, and interrupt state. Identify which component
   owns the mismatching edge.
4. Change that component's smallest relevant boundary. Do not add a global CPU
   delay to compensate for a peripheral read edge unless independent evidence
   shows the CPU timing is wrong.
5. Check a focused public-interface test, the original reproducer, the nearby
   passing regressions, and `make test`. If an established case regresses,
   discard the candidate and refine the cause before adding another adjustment.
6. Update this matrix with the result, limitation, and source links.

For example, `hblank_ly_scx_timing-GS` was fixed by moving the normal-line
readable LY edge from dot 454 to dot 452 while preserving the delayed LCD-start
edge. The extra HALT cycle candidate passed that ROM but failed six previously
passing timing ROMs, so it was discarded. See the [LY edge follow-up](#normal-line-ly-edge-follow-up-2026-10-25).


## Expanded PPU diagnostics: historical failures and current result

At the 2026-10-23 checkpoint, the default seven Mooneye checks passed, while
seven additional cases from the same local `mts-20240926-1737-443f6e1` archive
exposed six explicit failure signatures (not timeouts):

| Additional test | Result |
| --- | --- |
| `stat_irq_blocking` | PASS |
| `stat_lyc_onoff` | FAIL |
| `vblank_stat_intr-GS` | FAIL |
| `lcdon_timing-GS` | FAIL |
| `lcdon_write_timing-GS` | FAIL |
| `hblank_ly_scx_timing-GS` | FAIL |
| `intr_2_mode0_timing_sprites` | FAIL |

The expanded 14-case set now passes all 14 tests. The latest local report is
`build/test-roms/mooneye-expanded-final/report.md`; it records per-ROM logs
and SHA-256 hashes. Re-run it with:

```sh
python3 tools/run_mooneye_ppu_tests.py \
  --runner build/mooneye_rom_runner \
  --suite-dir /path/to/mts-20240926-1737-443f6e1 \
  --output-dir build/test-roms/mooneye-expanded \
  --extended
```

Investigate LCD enable/disable and LYC/STAT behavior first, then VBlank/STAT
interaction, and finally SCX and object-fetch timing. Consult each ROM's
source and hardware documentation before changing behavior. These failures
identify missing external coverage; they do not by themselves establish a
regression or prove which component is responsible.


### LCD-off LYC comparator correction (2026-10-24)

STAT's coincidence flag now retains the last active comparison while the LCD
is off; writing LYC during that time does not change the flag. Enabling the
LCD resumes comparison against LY=0 and preserves interrupt edge history so
an already-true match does not generate another request. Focused PPU tests
cover all four previous/next comparison combinations, flag reads, and IF.
The synthetic reset profile still initializes the flag to true for LY=LYC=0;
this does not claim hardware power-on accuracy.

At this intermediate stage, `make test` and the original seven Mooneye checks
passed; the expanded set had six failures. `stat_lyc_onoff` reached `Fail: r1 step 3` after passing
the two LCD-off checks; its remaining mismatch is consistent with the
unimplemented startup-line Mode 0 behavior. Do not mark the full ROM passed.
The follow-up below addresses the first scanline after LCD enable; its
access windows and timing are checked against the independent expectations in
[`stat_lyc_onoff.s`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/stat_lyc_onoff.s)
and [`lcdon_timing-GS.s`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/lcdon_timing-GS.s).


### LCD startup mode order (2026-10-24)

After LCD enable, the first scanline now reports Mode 0 before Mode 3;
subsequent visible lines use the normal Mode 2/3/0 sequence. The startup state
is reset on every LCD restart and is separate from the DMG post-boot profile.
PPU interface tests cover initial mode, Mode 2 interrupt suppression, OAM/VRAM
access, transition to Mode 3, and the following line's Mode 2 interrupt.

Together with the LCD-off comparator correction, this makes the unmodified
`stat_lyc_onoff` ROM pass (245,364 M-cycles). It is now in the default eight-test
Mooneye regression set. The expanded 14-test set has nine passes and five
remaining failures. `lcdon_timing-GS` and `lcdon_write_timing-GS` still fail:
startup dot timing remains approximate, so the mode-order fix does not imply
complete LCD-enable timing accuracy. The historical failure table above
records the initial diagnostic run, rather than the current result.


### LCD startup timing and memory edges (2026-10-24)

The unchanged `lcdon_timing-GS` and `lcdon_write_timing-GS` ROMs now pass.
The startup scanline begins two dots into the internal line timeline:
Mode 3 starts after 78 elapsed dots and baseline HBlank after 250. Readable
LY advances at elapsed dot 452; Mode 2 starts at 454. Subsequent lines retain
456-dot periods. The line counter readout, LYC comparison, and bus access
edges are modeled separately: during the final two dots readable LY has
advanced, coincidence is clear, and OAM reads are blocked before visible
Mode 2. VRAM reads block in the final two Mode 2 dots; OAM writes may pass
in that window although reads remain blocked. Focused public-interface tests
cover these read/write differences and the boundary dots. Existing pixel
emission tests now account for the startup line's two-dot offset; normal
Mode 3 still uses its existing 172-dot baseline.

Both ROMs join the default ten-case regression set. The expanded set has
11 passes and three remaining failures: `intr_2_mode0_timing_sprites`,
`hblank_ly_scx_timing-GS`, and `vblank_stat_intr-GS`. Earlier entries above
record intermediate results. These ROMs cover selected startup/visible-line
edges, not all VBlank, line-153, or mid-scanline behavior.


### Mode 2 STAT source at VBlank entry (2026-10-24)

On DMG, entering VBlank also clocks the enabled Mode 2 STAT source even
though the mode field reads 1. The core now requests that entry event at the
same dot as VBlank, respecting blocking by an already-high combined STAT
line. Focused PPU tests cover enabled/disabled sources, HBlank-source
blocking, the reported mode, and the absence of repeated entry events on
later VBlank lines. The hardware-verified
[`vblank_stat_intr-GS`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/vblank_stat_intr-GS.s)
ROM now passes (323,625 M-cycles), including its DIV-based comparison of
VBlank and STAT interrupt timing. It joins the default eleven-case set.
The expanded 14-case set now has twelve passes; sprite Mode 0 timing and
SCX/HBlank timing remain the two failures. The source event does not imply
that the PPU actually enters Mode 2 during VBlank.


### Sprite-to-Mode 0 timing (2026-10-25)

The official
[`intr_2_mode0_timing_sprites`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/intr_2_mode0_timing_sprites.s)
ROM checks the interval from the Mode 2 STAT interrupt to Mode 0 across many
sprite positions and counts, and lists DMG among its verified systems. LotusGB
now passes the full ROM (4,181,841 M-cycles). Focused PPU-interface tests
protect a single X=0 object boundary, the visible pixel stall, and ten X=10
objects entering Mode 0 at the tested boundary.

To match these observed boundaries, the model treats the first OBJ fetch on a
scanline as overlapping one dot of the initial BG fetch phase. Additional
X=0 objects at the same fetch position pay the flat six-dot cost. This is a
test-backed timing model, not a direct hardware measurement: [Pan Docs describes
X=0 as an 11-dot penalty](https://eldred.fr/pandocs/Rendering.html#obj-penalty-algorithm),
so the effective one-dot overlap remains a point to verify against independent
hardware evidence. At this timing checkpoint, the renderer had not yet added
BG/OBJ FIFO staging or per-dot sprite arbitration; the current coarse FIFO model
and its remaining limits are listed in the matrix above.

At this intermediate checkpoint the expanded 14-case PPU set passed 13 cases; `hblank_ly_scx_timing-GS` was still failing at 233,928 M-cycles. A later LY-edge fix resolved that case, and the current expanded set passes 14/14 as recorded at the top of this matrix. This checkpoint does not claim complete PPU timing accuracy.


### Post-boot IME and SCX investigation (2026-10-24)

The skipped-boot startup profile now disables IME at `$0100`, as specified by
[Pan Docs](https://github.com/gbdev/pandocs/blob/master/src/Interrupts.md).
The synthetic CPU test reset profile is unchanged. A system-interface test
sets IF and IE before EI, verifies that the handler has not run and SP is
unchanged, then confirms that EI permits service and RETI restores execution.
`make test` passes.

The SCX diagnostic exposed this independent startup error: IME had been true,
so the ROM could enter STAT handling during setup, before its explicit EI.
Fixing startup does not by itself pass `hblank_ly_scx_timing-GS`; the first
SCX=0 boundary still fails (233,928 M-cycles). A diagnostic experiment adding
one M-cycle on HALT wake passes that ROM but breaks five existing PPU timing
ROMs. That experiment was reverted. Keep the SCX failure open and investigate
interrupt sampling/entry and bus phase together; do not change the SCX delay
formula merely to match one test. The expanded set remains twelve passes
out of fourteen.


### SCX / HALT wake follow-up (2026-10-25)

This section records the 13/14 checkpoint before the normal-line LY edge
update below. The extra HALT wake cycle was rejected because it regressed six
other timing ROMs.

At this checkpoint after the sprite Mode 0 timing change, the production
model passed 13/14 expanded PPU diagnostics. The only failure was the official
[`hblank_ly_scx_timing-GS`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/hblank_ly_scx_timing-GS.s)
ROM. Its SCX=0 case expects two LY samples on opposite sides of the line
increment: the first sample returns the old line as expected, but the second
test iteration, with one more M-cycle of delay, still returns the old value.

Re-testing the extra-HALT-wake-M-cycle variant against this newer baseline
makes the SCX ROM pass but lowers the expanded result to 8/14. Six formerly
passing cases regress: `intr_1_2_timing-GS`, `intr_2_0_timing`,
`intr_2_mode3_timing`, `intr_2_mode0_timing`, `intr_2_oam_ok_timing`, and
`intr_2_mode0_timing_sprites`. The experiment is reverted. It does not tell
us which component owns the edge mismatch; keep the production CPU/PPU timing
unchanged until the STAT request, HALT wake, interrupt cycles, and LY bus read
are measured on one clock.


### Normal-line LY edge follow-up (2026-10-25)

A same-clock trace showed that the second SCX=0 LY bus read occurred at dot
452, while the model kept the previous readable LY value until dot 454. The
official [`hblank_ly_scx_timing-GS`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/hblank_ly_scx_timing-GS.s)
expects the LY boundary at that read. Its source documents the expected
51/50/49-cycle cases for `SCX & 7` values 0, 1–4, and 5–7, and reports DMG
hardware verification.

The correction is scoped to the PPU's readable LY edge on normal scanlines:
LY now advances at dot 452. LCD startup retains its dot-454 edge because the
official [`lcdon_timing-GS`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/lcdon_timing-GS.s)
checks a two-dot-late first line followed by normal timing. This avoids adding
a CPU HALT cycle that would shift unrelated PPU and interrupt events.

The line-153 test remains anchored to the first readable LY=153 value. Its
relative sequence is unchanged: at +4 dots LY is zero while LYC compares as
153, at +8 the comparison is blank, and at +12 it compares as zero. The
internal scanline still completes at dot 456 before Mode 2 begins.

The PPU unit test now checks that a normal line retains its old LY through dot
451 and exposes the next LY at dot 452 while Mode 0 remains active; it also
checks Mode 2 still begins at dot 456. `make test` passes, and the expanded
14-case Mooneye PPU set passes 14/14. The local report is
`build/test-roms/mooneye-expanded-final/report.md`; these external ROM checks
are not part of GitHub CI. This resolves the SCX/HBlank failure in the tested
DMG profile, without claiming cycle-perfect behavior for every PPU edge.


### Timer reload write window (2026-10-24)

The initial external timer run passed 11 of 13 Mooneye cases. Both failures
were reload-write tests: `tima_write_reloading` and `tma_write_reloading`.
The timer previously applied the write collision rules to the final T-cycle
*before* reload, then discarded the reload state immediately after copying TMA.

Separate the four-T-cycle pending-overflow delay from the following four-T-cycle
reload window. TIMA writes cancel a pending reload throughout the delay;
during the reload window they are ignored. TMA writes always update TMA and
also update TIMA while the reload window is active. The interrupt remains
requested at reload, four T-cycles after overflow. Public Timer-interface tests
cover writes at each offset from overflow through the end of the reload window.
All 13 original external timer ROMs now pass.

References: [Pan Docs timer overflow behavior](https://gbdev.io/pandocs/Timer_Obscure_Behaviour.html#timer-overflow-behavior),
[Mooneye TIMA write test](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/timer/tima_write_reloading.s),
and [Mooneye TMA write test](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/timer/tma_write_reloading.s).


### DMG OAM DMA source decoding (2026-10-24)

The first DMA-specific Mooneye run passed `basic` and `reg_read` but failed
`sources-GS`. The DMA engine used the CPU address map for every source page,
so `$FE00` read OAM and `$FF00` read I/O instead of the DMG's `$DE00`/`$DF00`
source aliases. Translate DMA source pages `$E0`–`$FF` to `$C0`–`$DF`, while
preserving the written FF46 value and leaving CPU mappings unchanged.

The original hardware-verified
[`sources-GS` ROM](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/oam_dma/sources-GS.s)
now passes. Public-interface unit tests cover all 32 high source pages, all
160 transferred bytes, register readback, and unchanged CPU I/O mapping.
This is DMG behavior; the ROM explicitly does not pass on CGB. The source
mapping correction does not implement transfer duration, CPU bus conflicts,
restart timing, or PPU/DMA contention. Those remain separate work, as described
in [Pan Docs](https://gbdev.io/pandocs/OAM_DMA_Transfer.html).


### Clocked OAM DMA and CPU arbitration (2026-10-24)

The baseline before this change passed three copy/register/source ROMs but
failed `oam_dma_start`, `oam_dma_timing`, and `oam_dma_restart`. The transfer
now advances on the shared system clock instead of copying inside FF46 writes.
Relative to the FF46 write at M=0, fresh DMA leaves OAM accessible at M=1,
then starts copying and blocks OAM at M=2. A restart keeps the old transfer
running at M=1 and begins the new source at M=2. After 160 byte transfers,
CPU OAM access resumes at the following M-cycle boundary.

DMA reads bypass CPU clock callbacks and CPU bus arbitration; ordinary CPU
reads/writes are arbitrated after device advancement. CPU OAM reads return
`$FF`, HRAM remains usable, and FF46 readback/restart remains accessible.
A basic DMG conflict model separates VRAM from the cartridge/WRAM bus;
conflicting reads see the last DMA byte and writes are discarded. Exact
write-induced corruption, PPU OAM-scan/rendering conflicts, and sub-M-cycle
behavior remain outside this implementation.

The existing copy/source unit tests now advance the transfer before checking
completed OAM. Additional tests cover partial progress, per-byte source reads,
start/end ownership, both source-bus categories, DMA callback bypass and
restart continuity. A CPU-driven system test copies a wait routine to HRAM,
starts DMA there, waits, returns to ROM, and reads both ends of OAM.

References: [Pan Docs DMA overview](https://gbdev.io/pandocs/OAM_DMA_Transfer.html),
[Mooneye startup](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/oam_dma_start.s),
[duration](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/oam_dma_timing.s),
and [restart](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/oam_dma_restart.s).
[SameBoy's memory implementation](https://github.com/LIJI32/SameBoy/blob/master/Core/memory.c)
was also consulted to distinguish DMG source-bus conflicts from the simplified
HRAM-only usage guideline; it is a comparison reference, not independent
hardware validation of every contention edge.


### GBIT comparison and CI failure propagation (2026-10-24)

The first DMA milestone CI run returned success while its GBIT log reported
243/244 ordinary opcodes: the reference still treated normal STOP as one byte.
The tester's keep-going mode also returned zero after logging mismatches.
The local sibling GBIT checkout predates the pinned EI fix, causing an
additional EI mismatch. These are reference/harness discrepancies, not evidence
that DMA changed CPU instruction semantics.

The user's fork now consumes the normal STOP padding byte and returns nonzero
for mismatches even in keep-going mode. Regressions check all 256 ignored STOP
padding values and inject a deliberately wrong CPU to verify error propagation.
CI builds and runs those regressions and uses pinned revision
`40c5247b818a7c524e3c3e75fe0fde900a4ee8e9`. LotusGB also uses fail-fast comparison;
a run against the old library now fails rather than silently passing. A correct
local run reports **244/244 ordinary and 256/256 CB opcodes**, with no mismatch.
This covers normal STOP in the single-instruction reference, not hardware
joypad/interrupt entry races or other exceptional STOP paths.

Source for normal two-byte STOP with ignored padding:
[Pan Docs](https://gbdev.io/pandocs/Reducing_Power_Consumption.html#using-the-stop-instruction).


### Unconnected serial baseline (2026-10-24)

SB/SC are now mapped to a serial component rather than unmapped `$FF` reads.
The component owns a separate free-running serial divider; do not clock it
from Timer's DIV counter. Internal transfers shift eight incoming high bits,
clear SC bit 7 and request IF bit 3 on completion. External transfers do not
complete with no peer attached. Unit tests check every bit boundary, partial
SB values, unused SC bits, interrupt timing/non-repetition, abort/restart, and
starting partway through the free-running clock period. CPU-driven system
tests check handler dispatch at `$0058`, HALT wake, RETI stack restoration,
and completion despite repeatedly resetting DIV.

This adds disconnected-port behavior, not multiplayer support. The clock
phase follows startup: the BIOS path begins four T-cycles into reset timing;
the synthesized post-boot profile uses phase `$1C8`, matching the low nine
bits of its `$ABC8` divider handoff value. This preserves a free-running serial
phase that is not restarted by SC writes or DIV resets.

On 2026-10-27, Mooneye's model-specific
`boot_sclk_align-dmgABCmgb` passed with both startup paths: fast startup at
183,421 M-cycles and the supplied DMG BIOS at 6,043,515 M-cycles. Before the
phase initialization, the ROM reached its assertion with mismatched counters
in both modes. The focused component tests now check both startup phases. This
is test-ROM evidence for the ROM's documented DMG ABC/MGB target, not an
independent physical-hardware measurement or a claim about other DMG revisions.

Reproduce from the repository root with the external Mooneye suite:

```sh
make -C src/lib mooneye-runner build_dir="$PWD/build"
build/mooneye_rom_runner ../test-roms/mooneye/acceptance/serial/boot_sclk_align-dmgABCmgb.gb
build/mooneye_rom_runner ../test-roms/mooneye/acceptance/serial/boot_sclk_align-dmgABCmgb.gb \
  --boot-rom "$HOME/Documents/RetroArch/system/dmg_boot.bin"
```

References: [Pan Docs serial transfer](https://gbdev.io/pandocs/Serial_Data_Transfer_(Link_Cable).html),
[AntonioND's cycle-accurate hardware notes, section 5.1](https://github.com/AntonioND/giibiiadvance/blob/master/docs/TCAGBD.pdf),
and [Mooneye serial boot alignment source](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/serial/boot_sclk_align-dmgABCmgb.s).


### Mooneye result-marker regression (2026-10-24)

The serial alignment diagnostic exposed a false failure at 490 M-cycles:
its counter loop temporarily has `$42` in B/C/D/E/H/L. The old runner treated
that register pattern alone as completion. The
[official protocol](https://github.com/Gekkio/mooneye-test-suite#passfail-reporting)
also requires an `LD B,B` breakpoint. The runner now checks signatures only
when a complete instruction has prefetched that marker from the selected
ROM's fixed reporting code. A is not part of the protocol signature.
Bank-switched reporting code and reporting from RAM/HRAM are outside this
runner's current scope.

`make test` now runs seven generated-ROM protocol probes, without external
ROM dependencies: temporary success/failure patterns, actual failure, an
unrelated marker, nonzero A on success, and `$40` as a CB opcode or immediate
operand. The temporary-failure probe failed before the runner correction.
All seven now pass. With corrected reporting, the default 40-case Mooneye
set still passes, and the expanded PPU set still has its same two known
failures (SCX/HBlank and object timing). The serial boot-alignment diagnostic
now reaches its genuine failure marker; its early 490-cycle result was not
valid evidence of hardware behavior.


### Common mapper acceptance (2026-10-24)

The available Mooneye mapper ROMs pass without changing the cartridge
implementation. The explicit `mapper` group selects only standard wiring:

| Mapper | Cases | Coverage |
| --- | ---: | --- |
| MBC1 | 12 | Four register/address tests, 8/32 KiB RAM, ROM sizes 64 KiB–2 MiB |
| MBC2 | 7 | Register/address and unused-bit tests, internal nibble RAM, ROM sizes 64–256 KiB |
| MBC5 | 8 | ROM banking across sizes 64 KiB–8 MiB, including the ninth bank bit |

The suite names use bit units: for example, `rom_16Mb` is 2 MiB and
`rom_64Mb` is 8 MiB. These tests support the existing capacity claims; the
emulator is not limited to 32 KiB when a supported mapper is present.

The combined `all` group now has 67 passing cases; the default `ppu` group
remains eleven. This optional external-ROM workflow is outside GitHub CI.
Reports include each selected ROM's SHA-256 for repeatability; the recorded
hash identifies the supplied file and does not verify it against an official
release checksum manifest. Logs retain the suite-relative directory names so
repeated names such as `rom_512kb` from different mappers cannot overwrite one another.

`multicart_rom_8Mb` is deliberately excluded: MBC1M uses different wiring and
remains outside the supported standard-MBC1 scope. The local archive has no
MBC3 mapper group; existing MBC3/RTC evidence remains the unit, frontend and
recorded game checks. Passing mapper ROMs is not a claim of full gameplay,
RTC disk persistence, all MBC5 RAM/rumble behavior or every cartridge variant.
The harness also continues to assume fixed reporting code as documented above.

References: [Mooneye mapper ROMs](https://github.com/Gekkio/mooneye-test-suite/tree/main/emulator-only),
[MBC1 register test](https://github.com/Gekkio/mooneye-test-suite/blob/main/emulator-only/mbc1/bits_bank1.s),
[MBC2 register test](https://github.com/Gekkio/mooneye-test-suite/blob/main/emulator-only/mbc2/bits_romb.s),
and [8 MiB MBC5 test](https://github.com/Gekkio/mooneye-test-suite/blob/main/emulator-only/mbc5/rom_64Mb.s).
Although these live in the `emulator-only` directory, the cited source files
report verification with configurable flash cartridges using genuine MBC chips.


### Final VBlank line: LY and comparison (2026-10-24)

The readable LY register resets to zero during line 153, before the scanline
and VBlank mode end. The old PPU kept LY at 153 until the end of that line,
which also delayed LYC=0 requests until the next frame. Keep internal scanline
progress separate from the readable counter so early LY=0 does not start
rendering, reset the window counter, or enter Mode 2 prematurely.

Public-interface tests anchor to the first readable LY=153 value and check
the following sampled states from the DMG timing model:

| Dots after the first readable 153 | Readable LY | LYC comparator | STAT mode |
| ---: | ---: | --- | ---: |
| 0 | 153 | No match | 1 |
| 4 | 0 | Compares as 153 | 1 |
| 8 | 0 | No match | 1 |
| 12 | 0 | Compares as 0 | 1 |

LYC=0 remains matched across frame rollover, so clearing IF during that
continuous match does not create a duplicate request at Mode 2 entry. Tests
also check that the active Mode 1 STAT source blocks a new LYC request, that
VBlank is not requested a second time, and that the full scanline completes
before the next frame starts. A CPU-driven system test accepts LYC=0 and reads
LY=0 with STAT still reporting Mode 1 from the handler.

References: [AntonioND's DMG timing tables, section 8.9.1, PDF pages 31–32](https://github.com/AntonioND/giibiiadvance/blob/master/docs/TCAGBD.pdf),
[Pan Docs LY reset discussion](https://github.com/gbdev/pandocs/issues/646),
and [SameBoy's last-line implementation](https://github.com/LIJI32/SameBoy/blob/master/Core/display.c).
The hardware notes sample at four-dot resolution; SameBoy was consulted to
cross-check the finer transitions in the current model, not as independent
hardware proof of every dot. These focused tests do not establish complete
STAT-write race or CGB timing accuracy; the local Mooneye archive has no
specific line-153 acceptance ROM.


At this checkpoint, `make build` and `make test` pass, including dmg-acid2 and
the generated Mooneye result-protocol probes. The combined default 67-ROM set
and corrected GBIT (244/244 ordinary + 256/256 CB) pass. Big2Small completes
3,600 frames with the same final frame hash and unchanged original SRAM. The
expanded PPU diagnostics remain 12/14, with the same SCX/HBlank and object
timing failures. No new external line-153 acceptance result is claimed.

## Window Y condition and hidden rows (2026-10-24)

The DMG window now remembers equality between WY and the internal visible
scanline at line start. Later WY writes cannot undo that condition or trigger
it retroactively by moving WY above the current line. VBlank clears it; LCD
restart resets both the condition and the window row counter. The condition
is tracked even while LCDC.5 is clear on DMG. Rendering and object-fetch
penalties use the same condition.

Public PPU tests cover retained equality after WY changes, off-screen WX
hiding without advancing the window row, frame reset, LCD restart, missed
line-start equality, and equality while the window is disabled.
This is a scanline-level correction, not complete mid-scanline window
emulation: WX=0/166 quirks, repeated window starts and precisely timed LCDC
changes remain outside this checkpoint.

Reference: [Pan Docs window rendering criteria](https://gbdev.io/pandocs/Window.html).

Validation: `make build`, `make test` (including dmg-acid2), all 67 default
Mooneye cases and corrected GBIT (244/244 ordinary, 256/256 CB) pass.
Big2Small completes 3,600 frames with the same final frame SHA-256 and the
original player SRAM unchanged. These existing ROM regressions supplement
the focused window tests; they are not independent coverage of every window
edge case.

## Second homebrew gameplay profile (2026-10-24)

The optional local homebrew script now supports the original Tobu Tobu Girl
in addition to the default Big2Small profile. A 3,600-frame clean-SRAM run
publishes 3,600 video callbacks, 2,893,139 stereo audio frames and an 8 KiB
SRAM block. Its inspected final frame shows an action scene with character,
enemies and HUD; a neutral-input run remains in the opening scene.
The initial input attempt pressed START too early and stayed in the opening;
repeated START presses during opening scenes followed by stage confirmation
corrected the test procedure, without changing emulator code.

The final RGB SHA-256 is
`260f1d3926acb5a2db9b2ec757b6a493158dc4f9de3b98b7e939df484e35c8f4`.
This is a recorded observation, not an enforced reference image or proof of
complete gameplay compatibility. See [the procedure, ROM hash and sources](homebrew-check.md#tobu-tobu-girl-gameplay-smoke-check).

## SCX coarse tile sampling (2026-10-24)

Background tiles now sample SCX high bits as they are fetched on the existing
simplified Mode 3 clock. SCX low-bit discard stays fixed from line start, and
object-behind-BG priority is resolved against the fetched color at output.
Focused public-interface tests fail on the old line snapshot and pass on the
new implementation. See [the scope and external images](ppu-scrolling.md).

In the archived emulator-reference image comparison, the SCX-high difference
fell from 6,426 to 84 pixels out of 23,040. The remaining pixels lie at
x=16–23 on selected rows; their hardware cause is not established. The BGP,
SCX-low and SCY image differences also remained unresolved. No global
bus/pixel phase adjustment was made to fit those images; they are not PPU
acceptance results.

## SCY bitplane sampling checkpoint (2026-10-24)

Split background tile-number, low-bitplane and high-bitplane reads into
separate stages. Each stage samples the relevant SCY bits; changing SCY after
the map read preserves that tile number. Public-interface tests protect writes
before/between/after bitplane reads and tile-map-row crossing. The preceding
implementation fails the new regression. See [the sampling model and traces](ppu-scrolling.md#separate-scy-bitplane-reads).

In that archived image comparison, the SCY difference fell from 9,873 to
8,330 pixels; SCX-high remained at 84, while BGP and SCX-low remained at 3,642
and 540. These counts are diagnostic only. The limited phase experiments were
not adopted. `make test`, all 67 default
Mooneye cases and GBIT (244/244 + 256/256) pass. Both 3,600-frame homebrew
profiles retain their previous final frame hashes, and the original supplied
Big2Small save is unchanged. This remains a staged baseline, not complete
FIFO/sprite arbitration or full raster-effect compatibility.
