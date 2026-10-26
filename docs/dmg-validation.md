# DMG implementation and validation matrix

This is the canonical status matrix for the current DMG target. “Implemented” means the behavior exists in LotusGB; it does not imply complete hardware accuracy. The table separates implementation, test evidence and known gaps. Dated investigation notes below preserve earlier failures and fixes; use the row summaries for current status.

The latest expanded PPU timing result (2026-10-25): all 14 cases pass, including
`hblank_ly_scx_timing-GS` and the sprite Mode 0 timing ROM. Earlier result
counts below are historical checkpoints.

The current system starts from a DMG post-boot profile and skips boot-ROM execution. CGB behavior and unsupported cartridge hardware are outside this matrix.

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
| CPU instructions and timing | CPU advances in M-cycles and reports T-cycles. Focused behavior includes DAA, CCF/SCF/CPL and accumulator rotates; one-cycle register-to-register `LD r,r`; per-M-cycle phases for `INC rr`, `DEC rr`, `ADD HL,rr`, `ADD SP,e8`, `LD HL,SP+e8`, `LD SP,HL`, `LD rr,d16` and `LD (a16),SP`; `JP HL` target-fetch timing; `LD r,n`, `LD r,(HL)` and `LD (HL),r/n` phases; absolute `LD A,(a16)` / `LD (a16),A` and high-page `LDH` A loads/stores; `LD A,(BC/DE)` and `LD A,(HL+/-)` read/update timing; `LD (BC/DE),A` and `LD (HL+/-),A` write/update timing; register and `(HL)` increment/decrement; CB register and `(HL)` rotate/shift/bit/reset/set operations; `BIT b,(HL)` read-only timing; 8-bit ALU instructions using registers, `(HL)` and immediate operands; stack/jump bus sequences; conditional JP taken/untaken timing for NZ/Z/NC/C, taken and untaken JR paths, and signed relative offsets; per-M-cycle SP updates for PUSH/CALL/RST and interrupt entry; POP/conditional-RET stack reads and branch timing; POP AF flag-byte masking and commit phase; EI delay; STOP/HALT distinction, HALT wake-up and HALT bug. Encountering one of the 11 base opcodes marked unsupported fails execution and reports the opcode/address through the libretro error log; this is an emulator diagnostic policy, not a claim about the hardware's lock-up behavior. | `make test`: `cpu_instruction_test.cpp`, `cpu_cycle_test.cpp`; tests cover unsupported base opcodes and the frontend error path. The `ADD SP,e8` test exhausts all SP low-byte/offset pairs; tests check ordered bus accesses, flag-update phases and target opcode fetches; all 49 register-to-register `LD r,r` pairs verify one-cycle transfer, destination-only update and unchanged flags; register `INC`/`DEC` tests cover all seven byte registers and check one-cycle Z/N/H updates with preserved C at zero/nibble boundaries; single-cycle `CCF`/`SCF`/`CPL`/accumulator-rotate tests check A/C results, flag preservation/clearing and the next fetch; CB register tests cover rotate/shift/`BIT`/`RES`/`SET` results, the second-opcode fetch phase, flag behavior and next fetch; `(HL)` RMW tests check nibble/byte boundaries and writes; CB `BIT` tests all bit positions with set/clear data and no memory write; all eight 8-bit ALU operations using `(HL)` and all eight using immediate operands check that reads precede A/flag updates and validate the next opcode fetch; register-form tests cover all eight operations and verify result/flag commit with the one-cycle following opcode fetch; indirect A loads and stores check the original bus address, HL wrap/update phase, later A commit for loads, A value for stores, unchanged flags, and next opcode fetch; `LD r,n` and `LD r,(HL)` cover all seven byte-register destinations, `LD (HL),r` all seven byte-register sources, and `LD (HL),n` checks immediate-read, write and following-fetch phases; absolute and high-page A loads/stores check operand/data bus order, address formation, A commit, unchanged flags and next fetch; PUSH AF checks high-byte A then low-byte F stack writes and per-M-cycle SP changes; conditional JP checks all four predicates on both paths; JR checks taken/untaken timing, signed-offset PC updates and target fetch; taken CALL cc checks every stack phase; POP AF checks that old AF survives both stack reads and a popped flag byte with nonzero lower bits is masked at commit; GBIT instruction integration runs separately and in CI. | These CPU phase checks follow GB-CTR and unit-test observable state; they are not physical-DMG timing measurements. The 11 listed opcodes are explicitly unsupported; their exact DMG behavior remains unverified. DMG OAM corruption from IDU accesses is not implemented, and other instruction-internal timing is not fully validated on hardware. Add a focused ROM only when a real disagreement or game exposes a gap. |
| Timer and interrupts | DIV/TIMA/TMA/TAC edge-based counting, delayed TIMA overflow reload and its four-T-cycle write window, DIV-write edges, IF/IE, interrupt priority and CPU interrupt entry. | `make test`: `timer_test.cpp` verifies TAC frequencies and confirms TIMA overflow requests Timer IF on reload; `game_boy_system_test.cpp` checks that the timer IRQ wakes HALT and reaches its handler; `cpu_instruction_test.cpp` covers CPU interrupt entry. | All 13 Mooneye `acceptance/timer` ROMs pass, including TIMA/TMA writes during reload. The PPU group includes interrupt-entry and HALT timing ROMs, and the interrupt group adds ten EI/DI, RETI, HALT and IF/IE checks. This does not cover every interrupt race. |
| Address bus and cartridge | Address mapping and WRAM echo; ROM-only, MBC1, MBC2, MBC3, and MBC5 banking. Special cases include MBC2's 512×4-bit RAM and MBC3 RTC latch, HALT, and day rollover. | `make test`: `address_bus_test.cpp`; 27 Mooneye standard MBC1/MBC2/MBC5 cases pass. Libretro tests cover save RAM/RTC memory-block restoration across reset and reload. | RetroArch `.srm` disk write/reload was manually verified with Big2Small on 2026-10-23. RetroArch `.rtc` disk write/reload was verified on 2026-10-26 using a temporary MBC3 timer-only probe: clean exit wrote the 24-byte RTC block, and a second RetroArch process restored and advanced it. These verify one cartridge/frontend path each, not every mapper or game; RTC evidence and limits are recorded under `build/test-roms/mbc3-rtc-persistence/`. Tobu Tobu Girl remains an MBC1 gameplay target; high-score persistence requires completing a level and is not a required manual acceptance step. MBC30-sized RAM and other mapper families are unsupported; MBC5 rumble banking works, but motor output is not exposed. |
| Serial, unconnected DMG | SB/SC registers, unused SC bits high, free-running 8192 Hz internal bit clock, MSB-first shifting with incoming ones, eight-bit completion, serial IF request, abort and externally clocked waiting. Serial clock is independent of DIV and continues in HALT. | `make test`: `dmg_serial_test.cpp` checks register and bit/completion boundaries; system tests check serial IRQ/HALT/RETI and continued completion despite repeated DIV resets. | No real peer, external clock input, accessories or CGB fast clock. The model-specific `boot_sclk_align-dmgABCmgb` ROM still fails; exact post-boot clock phase and SC-write edge races remain unresolved. Component/system tests do not establish full hardware serial conformance. |
| Joypad | Active-low button lines, direction/button selection, and falling-edge interrupt requests. | `make test`: `joypad_test.cpp`, `game_boy_system_test.cpp`; libretro input regression uses GB Pong. | STOP is not currently woken by joypad input. The libretro input/callback phase can shift VBlank-synchronized visible response by one callback; this is demonstrated with a synthetic ROM, not hardware-validated. See the [input-phase report](#libretro-input-phase-and-vblank-response-2026-10-26). |
| PPU rendering | 160×144 DMG shade framebuffer; background, window, and 8×8/8×16 objects; tile addressing, palettes, transparency, and DMG object priority. BG and OBJ output use separate 16-pixel FIFOs; BG fetches have named tile-map/bitplane phases. OAM descriptors and event timing are prepared at scanline entry; the OBJ low/high tile planes are read at separate boundaries during each coarse event, and the decoded row is then overlaid into the OBJ FIFO. During timed OBJ fetch, BG catch-up stops at Push or when the BG FIFO becomes non-empty; BG rows push only into an empty FIFO. LCDC.0/LCDC.1 and palette effects are sampled at pixel output; background-map select is sampled at each tile-map fetch. Background fetch and per-line object descriptors are retained while their display bits are clear, so a later re-enable can affect upcoming pixels. OAM selection uses the first ten Y-matching entries even when their X positions are off-screen. | `make test`: `dmg_ppu_test.cpp`, `game_boy_system_test.cpp`; focused PPU tests cover mid-line LCDC.0/LCDC.1 disable/re-enable, enabling after scanline preparation, unchanged Mode 3 duration with OBJ disabled, left-clipped OBJ output, off-screen-object selection limits, map changes, and `testObjectFetchStopsBackgroundCatchUpWhenBgFifoHasPixels`; `dmg-acid2` exact framebuffer regression passes. Optional `make test-gbmicrotest-ppu-latch` captures four model-scoped visual probes; see [GBMicrotest diagnostics](#gbmicrotest-ppu-latch-diagnostics). | Exact Mode 3 hardware timing at mid-scanline register-write boundaries remains unverified because there is no matching DMG hardware result for those image comparisons. Game-level behavior remains an acceptance target; see the [Canyon Racer effect check](homebrew-check.md#author-described-raster-effect-check). Canyon Racer produced identical final frames in two same-core, same-input 3,600-frame runs (0 differing pixels). An older 407-pixel difference was between historical captures whose metadata does not identify its cause; there is no expected hardware image. SameBoy and mGBA repeat under the same input script and match LotusGB's final SRAM. After shade normalization, their frames differ from LotusGB in 4,006 and 5,426 pixels, respectively; see the [three-core diagnostic](homebrew-check.md#three-core-raster-diagnostic-2026-10-25). The cross-core results are not hardware oracles. Exact FIFO push cadence, OBJ bus fetch/arbitration, and hardware output remain unverified; F-1 Race remains a later scrolling target. |
| PPU modes and Mode 3 timing | 456-dot scanlines and mode transitions; VBlank/STAT requests, final-line early LY reset and delayed LYC comparison, and mode-based VRAM/OAM access restrictions. Mode 3 uses a 172-dot baseline, adds `SCX & 7`, a six-dot visible-window penalty, and modeled 6–11-dot object-fetch stalls. BG and OBJ pixels pass through separate 16-pixel FIFOs; the first modeled OBJ fetch overlaps one dot with the initial BG fetch phase. During timed OBJ fetch, BG catch-up stops at Push or when the BG FIFO becomes non-empty; a row pushes only when the BG FIFO is empty. | `make test`: `dmg_ppu_test.cpp` checks the VBlank IF request and selected mode boundaries, access restrictions, SCX/window/object penalties, left-clipped OBJ output, and frame behavior. `game_boy_system_test.cpp` verifies a PPU-generated VBlank request wakes HALT and reaches the CPU handler. `make test-mooneye MOONEYE_SUITE_DIR=/path/to/mts-20240926-1737-443f6e1` runs eleven external timing ROMs and writes logs/report under `build/test-roms/mooneye/`; all eleven pass with CPU bus transactions at the M-cycle end. The optional expanded 14-case PPU set passes 14/14. | The normal startup profile skips boot-ROM execution, so the Nintendo-logo animation is not currently a boot test; the logo's down-scroll also polls LY rather than relying on the VBlank interrupt. The timing ROMs cover selected STAT/mode/OAM-access edges, not every PPU transition. Normal-line readable LY changes at dot 452; LCD startup retains its separately verified dot-454 edge. SCX high bits are sampled per background tile on a simplified fetch clock; SCY is sampled separately at tile-map/bitplane stages. The catch-up predicate follows Pan Docs; SameBoy's fetcher-state loop uses a different boundary, and the new unit regression only protects the Pan Docs hypothesis. Neither it nor the selected ROMs establish which predicate matches hardware. Exact BG push cadence and OBJ bus-fetch/FIFO arbitration remain incomplete. The one-dot first-fetch overlap is a model supported by the passing sprite-timing ROM, not independent hardware measurement; Pan Docs describes the X=0 penalty as 11 dots. Keep adding focused ROMs when visual or gameplay issues expose a specific gap. |
| OAM DMA | FF46 schedules 160 bytes at one byte per M-cycle after startup, supports delayed restart, and arbitrates CPU OAM/source-bus access; DMG DMA sources `$E0`–`$FF` mirror `$C0`–`$DF`, independently of CPU OAM/I/O mappings. | `make test`: `dmg_oam_dma_test.cpp`; six Mooneye OAM DMA cases pass: `basic`, `reg_read`, `sources-GS`, `oam_dma_start`, `oam_dma_timing`, `oam_dma_restart`. | Basic read contention is modeled; exact write-induced bus corruption, PPU/DMA rendering contention, and sub-M-cycle bus effects remain unmodeled. |
| APU and audio output | DMG pulse 1/2, wave 3, and noise 4; stereo routing and PCM output. Special CH3 behavior includes delayed sample fetch, fetch-cycle wave RAM access, and retrigger corruption. | `make test`: `dmg_apu_test.cpp`, timer/system integration tests, and libretro callback tests. Optional `make test-roms` runs [Blargg DMG sound tests](https://github.com/retrio/gb-test-roms/tree/master/dmg_sound) 09, 10, and 12; all three passed in the 2026-10-26 run against the current core. | The selected ROMs focus on CH3 wave RAM behavior, not full APU conformance. CH4 waveform quality still benefits from listening or comparison against DMG reference recordings. |
| Startup, frame stepping, and libretro | Default startup skips the boot ROM and begins at the DMG post-boot state at `$0100` with IME disabled. `stepFrame()` advances a nominal 70,224 T-cycles. The libretro core provides video, audio, input, save RAM, and MBC3 RTC memory interfaces; runtime callback exceptions are logged and fault execution until reset or reload. | `make test`: system and libretro tests include content-error diagnostics, STOP warnings, and callback-exception-boundary coverage; `dmg-acid2` and GB Pong regressions; the core has been loaded in Ubuntu RetroArch and common-game play has been checked manually. | Reserved-opcode diagnostics remain `stderr`-only, and placeholder handling continues execution. Boot-ROM execution and model-specific power-on behavior are not implemented. Continue testing selected games end to end; do not treat core loading alone as full compatibility. |

The Mooneye `manual-only/sprite_priority` DMG frame also matches its v7.0
reference image exactly (0/23,040 pixels differ). This verifies sprite-priority
composition for that scene, not per-dot OBJ fetch timing; see the
[reproducible visual check](ppu-scrolling.md#mooneye-manual-sprite-priority-image-2026-10-25).

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

## Test commands

- `make test` runs the in-repository CPU, hardware, visual, and libretro tests.
- `make test-roms` optionally runs the selected Blargg CH3 ROMs when they are available outside the repository. This is not part of GitHub CI.
- `make test-mooneye MOONEYE_SUITE_DIR=/path/to/extracted-suite` optionally runs eleven selected Mooneye PPU/interrupt timing ROMs. The suite stays outside the repository; logs and a Markdown report go under `build/test-roms/mooneye/`. This is not part of GitHub CI.
- `make test-mooneye-visual` optionally compares the Mooneye `manual-only/sprite_priority` DMG frame with its v7.0 reference image. Extract the [c-sp test-ROM v7.0 bundle](https://github.com/c-sp/game-boy-test-roms/releases/tag/v7.0) outside the repository; the default path is `../test-roms/game-boy-test-roms-v7.0/mooneye-test-suite/`. It requires the optional Pillow venv described in the [PPU visual-check instructions](ppu-scrolling.md#external-visual-baseline). The report and images go under `build/test-roms/mooneye-sprite-priority/`; this is not part of GitHub CI and checks sprite priority, not exact fetch timing.
- `make test-mooneye MOONEYE_GROUP=timer MOONEYE_SUITE_DIR=/path/to/extracted-suite` runs all 13 timer cases; `MOONEYE_GROUP=all` runs eleven default PPU/interrupt cases, thirteen timer cases and six OAM DMA cases, ten CPU interrupt cases and 27 standard mapper cases (67 total). Reports go under `build/test-roms/mooneye-timer/` and `mooneye-all/`, respectively. These optional groups remain outside CI.
- `make test-mooneye MOONEYE_GROUP=dma MOONEYE_SUITE_DIR=/path/to/extracted-suite` runs the six OAM DMA cases; its report goes under `build/test-roms/mooneye-dma/`. These check copying, register readback, source mapping, start/end timing and restart timing; they do not establish full bus-corruption or rendering-contention accuracy.
- `make test-mooneye MOONEYE_GROUP=interrupt MOONEYE_SUITE_DIR=/path/to/extracted-suite` runs ten EI/DI, RETI, HALT and IF/IE ROMs, with a report under `build/test-roms/mooneye-interrupt/`. See [the interrupt checkpoint](interrupts.md#cpu-interrupt-acceptance-checkpoint-2026-10-24) for scope.
- `make test-gbmicrotest-ppu-latch GBMICROTEST_SUITE_DIR=/path/to/gbmicrotest` captures GBMicrotest's four 800-series PPU latch probes through the libretro core. Clone [aappleby/gbmicrotest](https://github.com/aappleby/gbmicrotest) outside the repository; the default suite path is `../test-roms/gbmicrotest/`. ROM and core checksums, callback summaries, RGB/PPM frames, and logs are written to `build/test-roms/gbmicrotest-ppu-latch/`. This is a visual diagnostic only; it does not compare frames to an expected image or enter CI.
- `make test-mooneye MOONEYE_GROUP=mapper MOONEYE_SUITE_DIR=/path/to/extracted-suite` runs 27 standard MBC1/MBC2/MBC5 cases; logs and the report go under `build/test-roms/mooneye-mapper/`. Each report records the ROM SHA-256. MBC1M is excluded because it is not implemented.
- `make test-homebrew` runs the [local Big2Small smoke check](homebrew-check.md), optionally with `HOMEBREW_SAVE=/path/to/big2small.srm`.
- The GBIT integration executable is a separate CPU comparison check; see the GBIT section in the README for its local command.

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

This adds disconnected-port behavior, not multiplayer support. The initial
phase is a deterministic synthetic profile, not a claim about physical
DMG/MGB boot timing. In particular, the model-specific Mooneye
`boot_sclk_align-dmgABCmgb` ROM targets that boot-phase detail and is separate
from the existing default regression set. It still fails with the current
profile (183,809 M-cycles at the actual result marker); do not claim independent
hardware ROM acceptance for serial timing from the component/system tests.

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
