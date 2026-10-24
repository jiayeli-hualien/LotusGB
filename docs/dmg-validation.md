# DMG implementation and validation matrix

This matrix summarizes the current DMG target and the evidence for each area. “Implemented” means the behavior exists in LotusGB; it does not imply complete hardware accuracy. Unit tests protect selected observable cases, while external ROMs and real-game checks provide independent integration evidence.

The current system starts from a DMG post-boot profile and skips boot-ROM execution. CGB behavior and unsupported cartridge hardware are outside this matrix.

| Area | Implemented behavior and notable DMG details | Current validation | Known gaps / next useful check |
| --- | --- | --- | --- |
| CPU instructions and timing | CPU advances in M-cycles and reports T-cycles. Focused behavior includes DAA and `ADD SP,e8` flags, stack/jump bus sequences, EI delay, interrupt entry, STOP/HALT distinction, HALT wake-up and HALT bug. | `make test`: `cpu_instruction_test.cpp`, `cpu_cycle_test.cpp`; GBIT instruction integration runs separately and in CI. | This is not a claim that every instruction timing edge is independently verified on hardware. Add a focused ROM only when a real disagreement or game exposes a gap. |
| Timer and interrupts | DIV/TIMA/TMA/TAC edge-based counting, delayed TIMA overflow reload and its four-T-cycle write window, DIV-write edges, IF/IE, interrupt priority and CPU interrupt entry. | `make test`: `timer_test.cpp`, `game_boy_system_test.cpp`, `cpu_instruction_test.cpp`. | All 13 Mooneye `acceptance/timer` ROMs pass, including TIMA/TMA writes during reload. The PPU group includes interrupt-entry and HALT timing ROMs, and the interrupt group adds ten EI/DI, RETI, HALT and IF/IE checks. This does not cover every interrupt race. |
| Address bus and cartridge | Address mapping and WRAM echo; ROM-only, MBC1, MBC2, MBC3, and MBC5 banking. Special cases include MBC2's 512×4-bit RAM and MBC3 RTC latch, HALT, and day rollover. | `make test`: `address_bus_test.cpp`; libretro tests cover save RAM/RTC memory-block restoration across reset and reload. | RetroArch `.srm` disk write/reload was manually verified with Big2Small on 2026-10-23: the user confirmed progress survived a restart. This covers an MBC5 + RAM + battery cartridge; it does not establish disk persistence for every mapper. Tobu Tobu Girl remains an MBC1 gameplay target; high-score persistence requires completing a level and is not a required manual acceptance step. Actual `.rtc` disk persistence also remains unverified. MBC30-sized RAM and other mapper families are unsupported; MBC5 rumble banking works, but motor output is not exposed. |
| Joypad | Active-low button lines, direction/button selection, and falling-edge interrupt requests. | `make test`: `joypad_test.cpp`, `game_boy_system_test.cpp`; libretro input regression uses GB Pong. | STOP is not currently woken by joypad input. Verify controls interactively on each target platform as platform work begins. |
| PPU rendering | 160×144 DMG shade framebuffer; background, window, and 8×8/8×16 objects; tile addressing, palettes, transparency, and DMG object priority. Palette registers are sampled as pixels are emitted. | `make test`: `dmg_ppu_test.cpp`, `game_boy_system_test.cpp`; `dmg-acid2` exact framebuffer regression passes. | `dmg-acid2` does not cover every mid-scanline effect. F-1 Race remains a scrolling/visual compatibility target. |
| PPU modes and Mode 3 timing | 456-dot scanlines and mode transitions; VBlank/STAT requests and mode-based VRAM/OAM access restrictions. Mode 3 uses a 172-dot baseline, adds `SCX & 7`, a six-dot visible-window penalty, and modeled 6–11-dot object-fetch stalls. | `make test`: `dmg_ppu_test.cpp` checks selected mode boundaries, access restrictions, SCX/window/object penalties, and frame behavior. `make test-mooneye MOONEYE_SUITE_DIR=/path/to/mts-20240926-1737-443f6e1` runs eleven external timing ROMs and writes logs/report under `build/test-roms/mooneye/`; all eleven pass with CPU bus transactions at the M-cycle end. | The timing ROMs cover selected STAT/mode/OAM-access edges, not every PPU transition. Mid-Mode-3 scroll/tile-fetch changes are also not modeled. Keep adding focused ROMs when visual or gameplay issues expose a specific gap. |
| OAM DMA | FF46 schedules 160 bytes at one byte per M-cycle after startup, supports delayed restart, and arbitrates CPU OAM/source-bus access; DMG DMA sources `$E0`–`$FF` mirror `$C0`–`$DF`, independently of CPU OAM/I/O mappings. | `make test`: `dmg_oam_dma_test.cpp`; six Mooneye OAM DMA cases pass: `basic`, `reg_read`, `sources-GS`, `oam_dma_start`, `oam_dma_timing`, `oam_dma_restart`. | Basic read contention is modeled; exact write-induced bus corruption, PPU/DMA rendering contention, and sub-M-cycle bus effects remain unmodeled. |
| APU and audio output | DMG pulse 1/2, wave 3, and noise 4; stereo routing and PCM output. Special CH3 behavior includes delayed sample fetch, fetch-cycle wave RAM access, and retrigger corruption. | `make test`: `dmg_apu_test.cpp`, timer/system integration tests, and libretro callback tests. Optional `make test-roms` runs [Blargg DMG sound tests](https://github.com/retrio/gb-test-roms/tree/master/dmg_sound) 09, 10, and 12; all three passed in the recorded 2026-10-22 run. | The selected ROMs focus on CH3 wave RAM behavior, not full APU conformance. CH4 waveform quality still benefits from listening or comparison against DMG reference recordings. |
| Startup, frame stepping, and libretro | Default startup skips the boot ROM and begins at the DMG post-boot state at `$0100` with IME disabled. `stepFrame()` advances a nominal 70,224 T-cycles. The libretro core provides video, audio, input, save RAM, and MBC3 RTC memory interfaces. | `make test`: system and libretro tests; `dmg-acid2` and GB Pong regressions; the core has been loaded in Ubuntu RetroArch and common-game play has been checked manually. | Boot-ROM execution and model-specific power-on behavior are not implemented. Continue testing selected games end to end; do not treat core loading alone as full compatibility. |

## Test commands

- `make test` runs the in-repository CPU, hardware, visual, and libretro tests.
- `make test-roms` optionally runs the selected Blargg CH3 ROMs when they are available outside the repository. This is not part of GitHub CI.
- `make test-mooneye MOONEYE_SUITE_DIR=/path/to/extracted-suite` optionally runs eleven selected Mooneye PPU/interrupt timing ROMs. The suite stays outside the repository; logs and a Markdown report go under `build/test-roms/mooneye/`. This is not part of GitHub CI.
- `make test-mooneye MOONEYE_GROUP=timer MOONEYE_SUITE_DIR=/path/to/extracted-suite` runs all 13 timer cases; `MOONEYE_GROUP=all` runs eleven default PPU/interrupt cases, thirteen timer cases and six OAM DMA cases and ten CPU interrupt cases (40 total). Reports go under `build/test-roms/mooneye-timer/` and `mooneye-all/`, respectively. These optional groups remain outside CI.
- `make test-mooneye MOONEYE_GROUP=dma MOONEYE_SUITE_DIR=/path/to/extracted-suite` runs the six OAM DMA cases; its report goes under `build/test-roms/mooneye-dma/`. These check copying, register readback, source mapping, start/end timing and restart timing; they do not establish full bus-corruption or rendering-contention accuracy.
- `make test-mooneye MOONEYE_GROUP=interrupt MOONEYE_SUITE_DIR=/path/to/extracted-suite` runs ten EI/DI, RETI, HALT and IF/IE ROMs, with a report under `build/test-roms/mooneye-interrupt/`. See [the interrupt checkpoint](interrupts.md#cpu-interrupt-acceptance-checkpoint-2026-10-24) for scope.
- `make test-homebrew` runs the [local Big2Small smoke check](homebrew-check.md), optionally with `HOMEBREW_SAVE=/path/to/big2small.srm`.
- The GBIT integration executable is a separate CPU comparison check; see the GBIT section in the README for its local command.

## Validation rule

Keep this matrix scoped to implemented behavior and the active DMG target. When a feature changes, update its row and add the narrowest useful regression. Add external ROMs when they provide evidence that a unit test cannot, rather than trying to pass every available test suite before continuing game development.


## Expanded PPU diagnostics

On 2026-10-23 the default seven Mooneye checks passed again. An additional
seven cases from the same local `mts-20240926-1737-443f6e1` archive exposed
six explicit failure signatures (not timeouts):

| Additional test | Result |
| --- | --- |
| `stat_irq_blocking` | PASS |
| `stat_lyc_onoff` | FAIL |
| `vblank_stat_intr-GS` | FAIL |
| `lcdon_timing-GS` | FAIL |
| `lcdon_write_timing-GS` | FAIL |
| `hblank_ly_scx_timing-GS` | FAIL |
| `intr_2_mode0_timing_sprites` | FAIL |

Run the combined set for diagnosis; it currently returns a nonzero status.
Keep its report separate from the default regression report:

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
