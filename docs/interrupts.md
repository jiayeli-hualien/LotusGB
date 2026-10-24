# DMG interrupt behavior and validation

This is the interrupt contract and implementation audit for LotusGB, as of
2026-10-25. It separates hardware rules from current implementation evidence.
Passing instruction tests does not establish peripheral-to-CPU timing accuracy.

## Request, wake, and service

`IF` (`$FF0F`) records requests; `IE` (`$FFFF`) selects enabled sources.
IME gates handler entry. Pending requests are `IF & IE & $1F`; peripherals
can set IF while IME or IE is clear. A request bit is a latch, not a count.
Software may also write IF. Multiple requests use the following priority:

| Priority | Bit / mask | Source | Vector | LotusGB request producer |
| --- | --- | --- | --- | --- |
| 1 | 0 / `$01` | VBlank | `$0040` | PPU |
| 2 | 1 / `$02` | LCD STAT | `$0048` | PPU combined STAT signal and DMG VBlank-entry event |
| 3 | 2 / `$04` | Timer | `$0050` | Timer overflow reload |
| 4 | 3 / `$08` | Serial | `$0058` | Serial hardware is not implemented; IF can be requested manually |
| 5 | 4 / `$10` | Joypad | `$0060` | Selected joypad input-line falling edge |

```mermaid
flowchart TD
    A[Peripheral event or software IF write] --> B[IF request latched]
    B --> C{Any enabled request: IF AND IE?}
    C -- No --> D[No interrupt wake or handler entry]
    C -- Yes --> E[Wake HALT if sleeping]
    E --> F{IME enabled and instruction boundary?}
    F -- No --> G[Retain request; continue execution when awake]
    F -- Yes --> H[Select highest priority request]
    H --> I[Clear IME and acknowledge selected IF bit]
    I --> J[Push return PC and enter vector]
```

This diagram describes HALT, not STOP. It omits subcycle races during entry.

## CPU rules

The hardware rules below follow [Pan Docs: Interrupts](https://gbdev.io/pandocs/Interrupts.html)
and [HALT](https://gbdev.io/pandocs/halt.html).

- Game entry at `$0100` has IME clear.
- DI disables service; EI enables it after the following instruction completes.
- RETI restores PC and enables interrupts. RET alone does not enable them.
- Accept interrupts between complete instructions; a CB prefix and its opcode
  belong to one instruction.
- Entry clears IME and the selected IF request, pushes the return PC, and
  vectors in five M-cycles. Stack writes store the high byte first.
- HALT wakes on an enabled pending request, including with IME clear. With
  IME clear and a request already pending when HALT executes, the HALT bug
  suppresses the next opcode-fetch PC increment.

A CPU M-cycle is four T-cycles. The five-cycle entry duration is not by itself
proof of the exact HALT wake phase or intra-cycle IF acknowledgement timing.

## Current implementation and evidence

| Area | Current model | Evidence / limitation |
| --- | --- | --- |
| Startup | `GameBoySystem` post-boot profile sets IME=0. Synthetic CPU reset retains its existing IME=1 convention. | `testPostBootInterruptsWaitForEi` requests IF/IE before EI, checks no handler/stack change, then checks handler execution and return. Synthetic reset is test infrastructure, not a hardware power-on claim. |
| IF / IE | `InterruptRegisters` latches low-five-bit requests; IF reads upper bits high. IE stores its byte. Request generation is independent of IME. | CPU and system tests exercise software requests and real timer/PPU requests. |
| EI / DI | CPU defers EI until the next instruction completes; DI cancels a pending enable. | `testEiDelay` includes a multi-cycle following instruction and an already-pending request. Additional tests check EI; EI return PC and DI cancellation across following instructions; external EI/DI sequence ROMs pass. |
| Entry / priority | CPU selects the source at acceptance, acknowledges it immediately, then performs two idle M-cycles, two stack writes, and the vector fetch. | `testInterruptEntryPriorityStackAndVector` checks all five steps, return address, stack order and preservation of another IF bit. Priority changes and acknowledgement races during entry remain unverified. |
| Prefetch / CB | CPU compensates for its prefetched opcode when saving the return address. It does not accept an interrupt between CB bytes. | `testInterruptWaitsUntilCbInstructionCompletes`. Prefetch compensation is internal bookkeeping; software must see the correct return PC. |
| RETI | The instruction restores PC and enables IME. | `testRetiImmediatelyServicesAnotherPendingInterrupt` checks immediate service before executing the resumed opcode, stack address/order and IF acknowledgement. External `reti_intr_timing` and `reti_timing` pass. |
| HALT | CPU idles while the shared clock advances peripherals. Pending IF/IE wakes it; IME decides whether to enter a handler. | `testHaltWakeAndHaltBug`, `testStopDecodeAndHaltIdle`, and `testTimerInterruptWakesHaltAndRunsHandler`. Exact wake/entry phase is still under investigation. |
| STOP | CPU exposes a separate stopped state with zero elapsed CPU T-cycles; explicit wake resumes fetching. | CPU STOP tests and system frame-step exit coverage. Automatic joypad wake and full hardware STOP behavior are not implemented. |
| Peripheral timing | Timer, PPU and joypad own request generation; CPU owns selection/service. | Component tests plus system integration. Unconnected serial internal-clock completion now requests IF bit 3, with component and CPU/HALT system tests. Actual link peers and external clock input are unsupported. |
| Bus phase | `SystemClock` advances components to the end of an M-cycle before a CPU bus access. | Existing CPU bus-access tests and external timing ROMs. This is a model with limited validation, not a guarantee for every register conflict. |

CPU tests are in `src/unit_test/cpu_instruction_test.cpp`; system tests are in
`src/unit_test/game_boy_system_test.cpp`. The implementation boundaries are
`GBCPU`, `InterruptRegisters`, `SystemClock`, and each peripheral.

## STAT-specific behavior

STAT sources share one interrupt signal. Clearing IF while that signal stays
high does not create a new rising edge. Switching between active sources can
therefore block another request. The mode field, readable LY, LYC comparison,
and memory-access windows need not change on the same dot.

LotusGB now has public-interface tests for combined-source blocking, LCD-off
LYC retention, LCD-enable transitions, and the Mode 2 source event at VBlank
entry while the reported mode is 1. External evidence includes:

- [`stat_irq_blocking`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/stat_irq_blocking.s)
- [`stat_lyc_onoff`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/stat_lyc_onoff.s)
- [`vblank_stat_intr-GS`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/vblank_stat_intr-GS.s)
- LCD-enable read/write timing ROMs and the selected Mode 2/Mode 0 timing ROMs.

These selected ROMs pass; this does not cover every STAT-write glitch or
interrupt-source race. The final-line early LY reset and delayed comparison
now have PPU-interface and CPU-handler tests, as described in the validation
matrix; there is no dedicated external line-153 ROM in the local archive. See [the validation matrix](dmg-validation.md)
for commands and current results.

## Open audit and investigation order

1. **SCX / HBlank timing conflict.** The original SCX ROM still fails its
   SCX=0 boundary after correcting startup IME. In the current 13/14 model,
   an extra HALT wake M-cycle makes it pass but regresses six previously
   passing PPU timing ROMs, so that experiment was discarded. The trace shows
   the first expected-old-LY sample succeeds, while the second test iteration
   (sampled with one more M-cycle of delay) still reads the old LY. Capture the
   STAT request dot, HALT wake, each entry cycle,
   vector fetch, and final LY bus-read phase against the same clock before
   changing CPU or PPU behavior. See the [follow-up experiment](#scx-halt-wake-follow-up-2026-10-25).
2. **EI combinations.** EI followed by EI now has focused pending-request
   coverage and passes the external `ei_sequence` ROM. EI followed by HALT
   with a later-arriving VBlank passes `halt_ime0_ei`. An already-pending
   request at EI/HALT still needs separate evidence; its return-PC behavior
   must not be inferred from ordinary HALT.
3. **Entry races and RETI.** Check changing IE/IF during the stack writes,
   cancellation/reprioritization of requests. Immediate service after RETI now
   has unit and external ROM coverage. Acceptance-time selection is still
   not proof of the remaining entry races.
4. **Peripheral-specific gaps.** Add evidence for STAT-write glitches. Line-153 early LY reset and comparison
   now have focused PPU and CPU-handler tests; finer hardware races remain open. Implement STOP wake with joypad evidence. Unconnected serial completion now
   has component/system coverage; exact serial boot phase and link peers remain separate work.

Items 2–4 are coverage gaps, not confirmed bugs or prerequisites for all other
emulator work. Choose an item when a reproducer or active feature needs it.

For a fix, keep the chain of evidence: hardware expectation → reproducer →
public-interface assertion → external regression. CPU instruction comparison,
component tests, and real-ROM integration answer different questions; retain
all relevant evidence instead of adjusting one subsystem to satisfy one ROM.

## Previous failures and what they taught us

| Incident | Layer | Guard against recurrence |
| --- | --- | --- |
| Interrupt accepted between CB bytes | CPU instruction boundary | Complete-prefixed-instruction regression |
| Wrong IME at skipped-boot entry | System startup | Pending IF/IE before EI system test |
| LYC comparison recomputed while LCD off | Peripheral request generation | Retained-flag and edge-history tests |
| Missing Mode 2 STAT event at VBlank | Peripheral event / shared signal | Entry-dot, blocking, and external DIV-based timing checks |
| SCX passes only after an extra HALT cycle | Cross-component timing, unresolved | Keep the conflicting ROM results; do not retain the experimental delay |


## Bounded SCX timing experiment (2026-10-24)

Four isolated library builds varied only the CPU bus-access phase and an
experimental extra IME-enabled HALT wake M-cycle. Production sources and
core behavior were not changed by the matrix run. Each build ran the same
14 unmodified Mooneye ROMs from `mts-20240926-1737-443f6e1`.

| Bus access phase within M-cycle | Extra HALT wake M-cycle | Passes / 14 | SCX / HBlank | Additional failures relative to current model |
| --- | --- | --- | --- | --- |
| End of T4 (current) | 0 | 12 | FAIL | None |
| End of T3 | 0 | 9 | FAIL | Mode 2→3, Mode 2→0, OAM access timing |
| End of T4 | 1 | 8 | PASS | Five existing PPU mode/interrupt timing cases |
| End of T3 | 1 | 11 | PASS | Mode 1→2 and Mode 2→0 interrupt interval |

The sprite timing ROM fails in all four builds. There is no uniformly better
setting in this matrix. Keep the current model and defer further SCX tuning
until there is new independent evidence, a more discriminating reproducer,
or a game compatibility issue. The result does not identify which subsystem
is ultimately wrong, nor does it show that interrupt handling is fully accurate.
Local experiment sources, logs and per-ROM reports are under
`build/timing-matrix/`; they are ignored build artifacts.


## CPU interrupt acceptance checkpoint (2026-10-24)

Ten additional unchanged Mooneye ROMs pass without changing CPU behavior:

| Area | ROMs |
| --- | --- |
| Delayed enable / cancellation | `ei_sequence`, `ei_timing`, `rapid_di_ei`, `di_timing-GS` |
| Return and immediate pending service | `reti_timing`, `reti_intr_timing` |
| HALT / IME combinations | `halt_ime0_ei`, `halt_ime0_nointr_timing`, `halt_ime1_timing2-GS` |
| Request / enable registers | `if_ie_registers` |

Run them with:

```sh
make test-mooneye MOONEYE_GROUP=interrupt \
  MOONEYE_SUITE_DIR=/path/to/extracted-suite
```

The standalone group reports to `build/test-roms/mooneye-interrupt/`; `MOONEYE_GROUP=all`
now includes these ten cases and totals 40 default checks. All 40 and `make test`
pass at this checkpoint. ROMs stay external.

References: [EI sequence](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ei_sequence.s),
[EI before HALT](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/halt_ime0_ei.s),
and [RETI interrupt timing](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/reti_intr_timing.s).
The EI/HALT ROM waits for a later interrupt; it does not establish the behavior
of every already-pending EI/HALT combination. No test expectations were relaxed.


## SCX / HALT wake follow-up (2026-10-25)

The current production model passes 13/14 expanded PPU diagnostics; only
[`hblank_ly_scx_timing-GS`](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/ppu/hblank_ly_scx_timing-GS.s)
fails. Its SCX=0 case samples LY twice after the Mode 0 STAT interrupt. The
first read correctly sees the old line (`$41`); the second test iteration uses
one more M-cycle of delay but still sees `$41` instead of `$42`.

An isolated experiment added one M-cycle after HALT wake before interrupt
entry. It made this ROM pass, but the expanded set fell to 8/14: six cases
that passed in the current model regressed (`intr_1_2_timing-GS`,
`intr_2_0_timing`, `intr_2_mode3_timing`, `intr_2_mode0_timing`,
`intr_2_oam_ok_timing`, and `intr_2_mode0_timing_sprites`). The CPU change was
reverted. This does not establish whether the remaining discrepancy is in
PPU event timing, interrupt acceptance, or the CPU bus sampling phase. Keep
HALT behavior unchanged until a same-clock trace isolates the event and read
edges; do not tune the global LY or Mode 0 boundary to compensate for this one
ROM.
