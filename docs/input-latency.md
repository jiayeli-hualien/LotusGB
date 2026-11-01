# Input response and gameplay acceptance

The default libretro core now uses bounded VBlank presentation (2026-11-01).
Tobu Tobu Girl's five required short/held actions respond in call 2 across
twelve tested states, compared with call 3 in the retained fixed-step
diagnostic baseline. Event tracing and a phase-only control establish the
callback/VBlank alignment cause for this bounded case. A real completed frame
or a bounded host repeat can end a call; STOP and CPU failure remain distinct.
Mac/Deck feel and physical input latency are deferred at the user's request;
they do not block the current automated investigation. Both stepping builds
now run the core/libretro regression gate, including audio cadence and repeated
rewind/replay. Their behavior during repeated LCD restarts is recorded below.

The current core preserves the tested one-poll inputs, but low-latency
fighting-game acceptance is **not established**. On 2026-10-31, GB Pong's
player paddle first responded in the second or third video callback counting
the input call. A continuously polling synthetic ROM responded in the input
call itself; a VBlank-only ROM responded in the next callback. These are
core/game observations, not physical button-to-screen measurements.

This closes the bounded automated M7 checkpoint in the [roadmap](roadmap.md).
Run-ahead remains separate. The shared hardware clock, complete-frame buffer
and version 1 state format are preserved. Earlier fixed-step measurements
below are historical baselines, reproducible with `make test-tobu-input-fixed`.

## Current presentation policy

`GameBoySystem::stepForPresentation()` returns one of `VBlank`, `Repeat`,
`CpuStopped` or `CpuFailure`, together with the actual T-cycles advanced.
It advances in M-cycles, preferring a real PPU completion and using a
70,224-T-cycle budget for a healthy repeat if no completion arrives. An
LCD-off/restart repeat submits the stable completed/blank image and actual
generated PCM. It is expected behavior and does not log a failure or request
a guest interrupt. STOP retains its explicit warning; faults/exceptions retain
the existing ABI containment and fault state.

This is a return/presentation policy, not an independent PPU/APU clock or a
guarantee of 70,224 T-cycles per call during LCD transitions. The native
reported rate remains approximately 59.7275 FPS. No frontend or installed
library is changed by building this policy. Headless tests cover the supported
contract; manual device feel and listening are still deferred.

## Reproducible automated baseline (2026-10-31)

Run `make test` to exercise both the new default and the fixed diagnostic
baseline. The following table records the earlier **fixed-step** baseline.
The two libretro regressions are
`testLibretroPongInputResponse` and
`testLibretroInputPulseAcrossLcdPhases` in
[`libretro_core_test.cpp`](../src/libretro/libretro_core_test.cpp).
They run in CI without downloads, external BIOS data or commercial content.
The measurement host was arm64 macOS with Apple Clang 21.0.0, optimized with
`-O2`. Both negotiated pixel formats produced the same results.

**Counting:** call 1 is the `retro_run()` that first polls the changed input;
call 2 is the following call. A result of 2 means one additional callback
boundary, not two extra buffered frames. One nominal DMG frame is
70,224 / 4,194,304 seconds, approximately 16.743 ms. Callback counts do not
measure elapsed wall-clock milliseconds: this harness runs without frontend
pacing or a display.

| Test, per pixel format | Press response | Release response |
| --- | --- | --- |
| GB Pong, 24 successive neutral checkpoints | Call 2: 20; call 3: 4 | Call 2: 19; call 3: 5 |
| Continuous JOYP palette probe, five LCD startup delays | Call 1: 5 | Call 2: 5 |
| VBlank-only JOYP palette probe, same delays | Call 2: 5 | Call 2: 5 |

For Pong, the pinned [MIT-licensed ROM](../tests/roms/gb-pong/README.md) starts
without a BIOS. After 60 neutral calls, each checkpoint is serialized and
restored for three branches: neutral, held Up, and Up for one input poll then
release. The comparison only examines the player's paddle strip, x=4–11,
y=0–143, excluding the ball and score. Press response is the first difference
between held Up and neutral; release response is the first difference between
the pulse and held Up, counted from the release call. A six-call observation
window bounds the search. Every short pulse first differs from neutral in the
same call as held Up. Restoring and advancing the neutral checkpoint by one
call supplies the next sample, covering calls 60–83 of one neutral timeline.
This is a bounded sample, not every possible game state or PPU phase.

The original synthetic ROMs select JOYP directions and map Up/release to
white/black palettes. They either read continuously or once each VBlank.
LCD startup delays of 1, 127, 512, 1024 and 2047 loop iterations vary the
PPU phase relative to the frontend call. After ten neutral calls, each probe
receives one Up poll followed by release. Every probe shows the pulse and
returns to black within the tested bounds. A partially changed completed
frame counts as response here; uniform transition timing is not asserted.

The harness changes Up **inside the input-poll callback**, checking that the
current sample reaches emulation. Regressions require one fresh input poll
and one video callback per measured Pong call, bounded response, preserved
pulses and no core errors. They do not pin the histograms as exact hardware
timing. The [earlier cross-core phase diagnostic](dmg-validation.md#libretro-input-phase-and-vblank-response-2026-10-26)
remains relevant; these measurements do not select a hardware-correct phase
or attribute all Pong delay to one component.

A temporary negative control moved input polling after `stepFrame()`. The
Pong release-response bound rejected it; the production order was restored.
Its local log is `build/input-latency-negative-control.log`. The ordinary
`make build`, `make test`, GBIT regressions and the pinned instruction
comparison passed (244/244 ordinary and 256/256 CB opcodes). The local gate
log is `build/input-latency-validation.log`; it also records an initial stale
negative-control executable and its forced rebuild before the passing run.

## Tobu Tobu Girl action baseline (2026-11-01)

Run the optional local measurement with the existing external pinned ROM:

```sh
make test-tobu-input-fixed
```

This builds the libretro test executable and runs
[`measure_tobu_input.py`](../tools/measure_tobu_input.py). The wrapper verifies
ROM SHA-256 `5d3871cae77db2287807e8914fd21f76203e73aa3fec20955489d45cb4571d8f`.
The ROM stays outside the repository; this optional mode is not run in CI.
It uses XRGB8888, fast startup without a BIOS and fresh in-memory SRAM, with
no installed frontend settings or player saves changed.

The opening inputs match the [existing smoke profile](homebrew-check.md#tobu-tobu-girl-gameplay-smoke-check):
five-call Start presses at 180, 360, 540, 720, 900, 1080, 1260 and 1440;
five-call A presses at 1620 and 1800. At checkpoints before zero-based calls
1840–1851, save a state and compare eight subsequent calls under neutral,
one-poll and held input. Each neutral replay must be frame-identical.

The observer compares x=40–119, y=40–135, the central region containing the
player. It excludes the right-hand HUD. Differences are between corresponding
frames restored from the same checkpoint, so ordinary enemy animation does
not count as response. A response can be a facing/animation change as well as
displacement; this measures first visible input effect rather than a specific
movement distance. Left and Right must produce distinct sequences, and each
Right+A/B pulse must differ from Right alone to detect a lost action button.

| Input | Held / one-poll first response, all 12 checkpoints | Pulse versus held after release |
| --- | --- | --- |
| Left, Right | Call 3 / call 3 | Difference in call 3 counted from release |
| B flap | Call 3 / call 3 | Difference in call 3 counted from release |
| Right+A dash | Call 3 / call 3 | No difference in the eight-call window |
| Right+B | Call 3 / call 3 | Difference in call 3 counted from release |
| A without direction, diagnostic only | No playfield difference in the window | No difference |

Directionless A is retained as an observation, not interpreted as a lost press
or a latency of zero. Likewise, a triggered dash continuing after release does
not prove that release was lost. Direction+A has an additional visible effect
over direction alone, and all five movement/action cases preserve the tested
one-poll pulse. These bounds cover this airborne scene, not depleted resources,
landings, other levels, arbitrary rapid command sequences or long-session feel.

`build/test-roms/tobu-input-fixed/report.json` contains the fixed test-executable hash,
command, individual results and histograms; `runner.log` retains failures.
Fresh measurements are staged before replacing evidence, and the success
report is removed before a new run. Generated PPMs retain all eight calls of
the first checkpoint for pulse, held and neutral branches. The reviewed
`contact-sheet.png` shows calls 3 and 8 in rows, with columns neutral, Left,
Right, B, Right+A and Right+B. The dash visibly displaces the character right;
the flap and directional effects also differ from neutral.

`make build`, `make test` and the pinned GBIT comparison passed after adding
this optional measurement (244/244 ordinary and 256/256 CB opcodes). The
local gate log is `build/test-roms/tobu-input/validation.log`. That initial
measurement did not change production core code or install a library.

### One-callback reference difference — reproduced baseline

A separate runner comparison uses the same external 256-byte DMG BIOS in
both cores, SameBoy forced to `Game Boy`, fresh SRAM and the same opening
script. At call 1840, a one-call Right, Right+A or B pulse is compared against
that core's own neutral run. All three first differ in the central playfield
at call 3 in LotusGB and call 2 in SameBoy. The neutral SameBoy capture was
visually checked to show active gameplay, not a blank/failed startup.

| Identity | SHA-256 |
| --- | --- |
| LotusGB library, unchanged production core | `93d9e02e5bce9ae0ea2bbec87b4f8dc9ba11bbb948d6ec7dcfe8cefe53f8f842` |
| SameBoy 1.0.4 `c87d256` library | `10b15b7acddd18414d066a4a90f7cf0ea0999ac1ffa66deec7d6ef609060977a` |
| External DMG BIOS | `cf053eccb4ccafff9e67339d4e78e98dce7d1ed59be819d2a1ba2232c6fce1c7` |

Local reproduction script, inputs, logs and raw captures are under
`build/test-roms/tobu-input/compare-reference.py`, `reference-lotus/`,
`reference-sameboy/` and `reference-report.json`. An earlier empty-system-
directory attempt explicitly failed to load SameBoy's BIOS; it is retained
under `reference-no-bios-invalid/` and is excluded from the comparison.

This is one game/checkpoint comparison and does not establish a constant
extra frame for every game or identify which phase matches hardware. The
mechanism is now traced below, extending the earlier
[VBlank/input phase observation](dmg-validation.md#libretro-input-phase-and-vblank-response-2026-10-26).
The presentation decision is now implemented in the current policy above.
The following trace retains the earlier fixed-default checkpoint. Manual
frontend acceptance remains deferred by the user.

### Root cause: completed-frame publication relative to the callback

**Confirmed for this case:** the extra video call comes from the callback's
PPU phase. It is not a whole-frame delay in JOYP delivery or game processing.
At callback 1840, LotusGB polls at approximately LY=99; SameBoy polls at
VBlank, LY=144. Both read Right in that same callback at LY=147 and store the
same game input byte at LY=148. Both then change the sprite's shadow-OAM X
coordinate at LY=11 of the following visible frame. That coordinate reaches
hardware OAM via DMA at the next VBlank.

| Event for the one-poll Right input | Fixed LotusGB | VBlank-aligned LotusGB control | SameBoy |
| --- | --- | --- | --- |
| Frontend input poll, callback 1840 | LY≈99 | LY=144 | LY=144 |
| First selected JOYP read, PC `$379D` | Callback 1840, LY=147, `$EE` | Same | Same |
| Game input write, PC `$06BA`, `$C0AA` | Callback 1840, LY=148, `$00 → $01` | Same | Same |
| Sprite shadow X write, PC `$36E8`, `$C001` | Callback 1840, LY=11, `$54 → $56` | Same | Same |
| Hardware OAM X becomes `$56`, `$FE01` | Callback 1841, LY=144 | Same callback, LY and emulated tick | DMA starts at LY=144; transfer bytes not instrumented |
| First changed working framebuffer at submission | Callback 1841 | Callback 1841 | Not instrumented |
| First changed completed frame / video callback | Callback 1842 (press call 3) | Callback 1841 (press call 2) | Callback 1841 (press call 2) |

The game's DMA precedes its input read in VBlank. Its updated shadow sprites
therefore wait for the next DMA. At fixed LotusGB callback 1841, the changed
sprite is already in the working image, but the callback is inside the visible
frame: the latest **completed** snapshot still predates that change. The
affected frame finishes at the following VBlank and is submitted in callback
1842. SameBoy's callback ends at VBlank and submits the affected complete
frame in callback 1841.

The causal control changes only the phase: immediately before neutral callback
1839, advance a diagnostic LotusGB copy to the actual next frame-completion
event, then resume the same fixed 70,224-T-cycle calls. It keeps the completed
buffer and the same input script. The JOYP, input-byte, shadow-X and hardware-
OAM-X changes occur at exactly the same emulated ticks as in the unaligned
LotusGB run, yet the response becomes call 2. For example, the input byte
changes at T=129234392, the shadow X at T=129242452, and hardware OAM X at
T=129303016 in both variants. The trace samples are M-cycle observations
(at most four T-cycles of timestamp granularity), not sub-cycle hardware traces.
Actual returned frame bytes determine the response; auxiliary live/completed
buffer hashes only locate the rendering stage.

The instrumented baseline's six callback images overlap byte-for-byte with
the earlier uninstrumented captures for both LotusGB and SameBoy. Trace sources,
the phase-only control, raw logs and `root-cause-report.json` are retained under
`build/test-roms/tobu-root-cause/`; `prepare_trace.py` builds independent source
copies and `run_trace.py` executes them. The installed SameBoy library is read
through its exported debug callbacks and is not modified. The downloaded
[Tobu source](https://github.com/SimonLarsen/tobutobugirl) explains its input and
shadow-sprite pipeline; the pinned ROM's executed addresses above supply the
direct evidence. SameBoy's [adapter](https://github.com/LIJI32/SameBoy/blob/v1.0.4/libretro/libretro.c)
calls [GB_run_frame](https://github.com/LIJI32/SameBoy/blob/v1.0.4/Core/gb.c),
which exits on a VBlank event.

This establishes a callback-count cause, not a physical 16.743 ms measurement
or a claim that LotusGB is always one frame late. It also does not justify
submitting the unfinished working buffer: that would reopen the earlier
frame-coherence/tearing defect.

### VBlank stepping and fixed-step diagnostics

The system now exposes `GameBoySystem::stepUntilVBlank(maxTCycles)`. It advances
in whole CPU M-cycles until the PPU actually copies a completed visible frame
at VBlank entry, then returns `frameCompleted = true`. Starting in VBlank waits
for the **next** completion. The default budget is 70,224 T-cycles. LCD-off
budget exhaustion or STOP returns `frameCompleted = false`; CPU failures and
exceptions remain observable. Budgets must be positive multiples of four.
The completion notification is host bookkeeping, cleared each M-cycle and
excluded from snapshots; save-state format version 1 is unchanged.

Do not implement this by waiting for readable `LY == 144`. The PPU exposes
the next LY four dots before actual line rollover. A first diagnostic control
stopped there and still took three calls, because it remained just before
VBlank; the completion-event control above takes two. Focused PPU/system tests
cover that four-dot distinction, repeated VBlank stepping, LCD-off timeout,
STOP versus HALT, exception propagation and continuation after state restore.

Compare the default and diagnostic builds using the pinned external game:

```sh
make test-tobu-input
make test-tobu-input-fixed
```

The first target tests the new ordinary VBlank presentation build. The second
builds `build/libretro_fixed_input_test` with `LOTUSGB_TEST_FIXED_FRAME_STEP`.
The older `make test-tobu-input-vblank` name remains an explicit alias for
testing the default mode in its own output directory. None installs a core.
For the twelve Tobu states and five required action cases,
the default build responds in call 2 for short and held input, and the
Right+A/B combinations have an additional effect over Right alone. Output is
under `build/test-roms/tobu-input/` and `build/test-roms/tobu-input-fixed/`;
`measurements.json` records the
step mode. These startup paths reach different frame phases and can reach
different game states at a given call index; use the phase-only trace control
above for causal attribution, not the two aggregate histograms alone.

After adding the completion-event API, `make build`, `make test` and the
pinned GBIT comparison passed (244/244 ordinary and 256/256 CB opcodes).
The unchanged fixed-step game baseline was also rerun and still reports
call 3 in all five required actions across twelve checkpoints. The local
gate log is `build/test-roms/tobu-root-cause/validation.log`.

Keep both stepping contracts for controlled comparisons. The automated
LCD/startup, audio, save-state and rewind-protocol checks below now run in both
builds. Any choice of default pacing must account for the LCD-restart case;
manual Mac/Deck operation is deferred rather than required before this work.

## Headless pacing and rewind assessment (2026-11-01)

These numeric tables retain the pre-promotion headless assessment. The new
default has the same VBlank/budget return timing, with budget completion now
classified as an expected host Repeat instead of a diagnostic warning.

The user deferred Mac/Deck manual operation. Automated validation can proceed
with `make test` (both builds) or `make test-vblank` (diagnostic adapter only),
without downloaded ROMs. The existing committed MIT GB Pong and original
in-memory synthetic fixtures supply the inputs. Installed cores and frontend
settings are not changed by these checks.

Both builds pass the API, input, frame-coherence, audio, mapper persistence,
error-boundary and save-state tests. A new rewind-protocol regression records
30 Pong frame states, restores history positions 20, 10, 0 and 25, and replays
the original inputs. Every video image, PCM batch and final serialized state
must match; subsequent new input must still move the paddle. This is checked
in both pixel formats. It tests the core protocol, not a new RetroArch GUI
rewind session, RTC rewind or listening quality.

### Audio remains tied to actual hardware time

With LCD continuously enabled, the first VBlank call advances 65,664 T-cycles
and emits 751 stereo samples, while fixed stepping advances 70,224 and emits
803. Subsequent steady callbacks emit 803 or 804 samples at 48 kHz. Across
240 calls, totals are 192,823 for VBlank stepping and 192,875 for fixed
stepping, exactly matching their elapsed clocks. The initial partial interval
does not imply cumulative sample drift.

The new [original LCD-restart fixture](../tests/include/lcd_cadence_rom.hpp)
waits for VBlank, disables LCD, delays approximately 24K T-cycles, enables it,
increments an SRAM counter, and repeats. The system test compares cumulative
PCM frames to `floor(elapsedTCycles * 48000 / 4194304)` after **every** step.

| 120 calls, fast post-boot system test | Hardware T-cycles advanced | PCM stereo frames |
| --- | --- | --- |
| Fixed stepping | 8,426,880 | 96,437 |
| Bounded next-VBlank stepping | 5,705,488 | 65,294 |

The VBlank fixture reaches its one-frame budget without completion in 60 of
these calls, then reaches the actual event in a shorter following call. APU
sampling still exactly follows elapsed hardware clocks. At a strictly fixed
host call rate, however, that is less emulated hardware time per wall-clock
second. This exposes a presentation/scheduling question; the test does not
pad, stretch or discard PCM to make the totals look nominal.

The adapter test also runs 60 restart calls in each pixel format, requiring
one poll/video callback, CPU progress and coherent black/white frames. Fixed
stepping produces 48,218 PCM frames and samples 60 white images; VBlank
stepping produces 32,884 PCM frames and samples 30 black/30 white images.
The fixed callback does not intersect the brief interval between completion
of a black frame and LCD-off clearing the snapshot in this fixture. This is
retained as an observed sampling limitation; the test does not assert that a
fixed-rate callback observes every intermediate LCD state. These synthetic
results do not establish matching physical-DMG display behavior.

### SameBoy does not detach audio or hard-code 60 FPS

SameBoy 1.0.4 advances timer, APU and display from the CPU clock path in
[`GB_advance_cycles`](https://github.com/LIJI32/SameBoy/blob/v1.0.4/Core/timing.c).
Its [libretro adapter](https://github.com/LIJI32/SameBoy/blob/v1.0.4/libretro/libretro.c)
reports `GB_get_usual_frame_rate()`, which is clock rate / 70,224 for DMG,
approximately 59.7275 FPS. Generated samples accumulate in a buffer and are
uploaded in batches; the adapter does not require a fixed PCM count per image.
[`GB_display_run`](https://github.com/LIJI32/SameBoy/blob/v1.0.4/Core/display.c)
also produces periodic LCD-off/artificial presentation events, so
`GB_run_frame()` can return without a normal newly drawn PPU frame.

A local comparison uses the same original restart ROM and a minimal original
256-byte boot stub for all three libraries. The runner requests 48 kHz and
checks each core's reported AV timing before comparing sample counts:

| 120 callbacks, same synthetic boot stub and ROM | Reported FPS / sample rate | PCM stereo frames | SRAM restart count |
| --- | --- | --- | --- |
| LotusGB fixed | 59.7275 / 48,000 | 96,437 | 89 |
| LotusGB diagnostic VBlank | 59.7275 / 48,000 | 65,294 | 60 |
| Installed SameBoy 1.0.4 | 59.7275 / 48,000 | 64,819 | 59 |

The startup differs from the fast-post-boot test above; comparisons are within
this table. The libraries are pinned by SHA-256 in the generated report.
SameBoy also has variable callback-time/sample totals on this stress input.
Its result does not prove either core is hardware-correct, that the two
implementations are identical, or that the frontend audio sounds normal here.
Uniform hardware clocks per callback alone cannot decide correctness.

For ordinary LCD-on operation, native frame duration is stable. A frontend
can buffer audio and [resample with dynamic rate control](https://docs.libretro.com/development/cores/dynamic-rate-control/)
to synchronize device audio and display clocks, including a display near
60 Hz. That is separate from emulated APU/PPU timing. Buffering can absorb
transients; it cannot by itself fix a persistent production/consumption
mismatch. The user's actual RetroArch sync/refresh settings were not inspected
in this headless assessment, and no listening result is claimed.

### Current decision and remaining local work

The headless regression gate passes for both stepping contracts. Keep the
new bounded VBlank presentation default and the explicit fixed-step diagnostic
baseline. The production policy now distinguishes repeated/blank presentation
from a real PPU completion and from CPU STOP/failure, without synthesizing
guest VBlank IRQs or detaching clocks. A fixed-policy checkpoint resumes under
the new policy at the same hardware event with matching state and PCM; no
state migration is required. The reference source/result above is diagnostic
evidence for the design, not a hardware oracle.

After this promotion, `make build`, `make test` and the pinned GBIT comparison
form the required gate. `make test` exercises both policies, repeated
rewind/replay, coherent frames, LCD restarts, STOP/fault boundaries and audio
totals. `make test-tobu-input` measures the new default; the fixed diagnostic
profile retains its call-3 result. Current artifacts and the validation report
are under `build/test-roms/vblank-presentation/`. The earlier phase-trace
sources/logs retain their original fixed-default checkpoint; use the current
fixed diagnostic target to reproduce the old policy on current sources.

The new default also passed `make test-homebrew HOMEBREW_GAME=adjustris`
(full row, clear and next-piece control) and `make test-homebrew`
(Big2Small puzzle progress, fresh-process SRAM restore and continued input).
The restored 02% title and cleared-row images were visually reviewed. These
are local external-ROM checks; their reports retain the new library identity
`08cb4e57ed3a53958bdd57fea048375311564c600435d1f7fbd9b2b9bc6d8d5e`.
They preserve the bounded M1/M2 integration checkpoints, not full-game or
device/frontend acceptance.

Logs are under `build/test-roms/vblank-headless/validation.log`; the initial
over-strict fixed-step color-observation assertion and its correction precede
the passing gate. The synthetic-boot comparison's fixture, runner copy, inputs,
library identities and report are under `build/test-roms/vblank-headless/reference/`.
Default/diagnostic regression gates include the existing state-validation
failures and exception boundaries; no core failure is converted into success.

## What this means for playability

Existing [gameplay and save evidence](roadmap.md#established-baseline) supports
the selected personal-use DMG games. It does not establish complete GB
compatibility. Particular PPU races, cartridge variants and link play remain
outside the [validated scope](dmg-validation.md).

The input measurements show no lost one-poll Up pulses in these cases, but
the difference between a response in call 1 and call 3 matters for responsive
action games. A fighting game also has its own input polling, move startup,
animation and command-recognition windows. Its attack animation alone cannot
identify emulator-added delay. There is no universal frame-count threshold
that certifies every fighting game as playable.

Total physical latency additionally includes the controller transport and
polling, waiting for the frontend's next sample, emulation execution, video
queue/compositor, display scanout and pixel response. None of those external
costs is measured by this in-process test. Stable frame pacing and reliable
press/release sequences also matter to feel.

## Mac and Steam Deck acceptance procedure — deferred

Deferred by the user on 2026-11-01. This remains a later procedure for actual
device feel, audio and physical latency; it is not a prerequisite for the
current automated investigation.

1. Record the ROM/core SHA-256, frontend and OS version, controller connection,
   display refresh rate, video driver, shader, VSync/threaded-video settings,
   and rewind/run-ahead settings. Start with run-ahead disabled and reuse the
   current installed core; this change does not require a new core library.
2. Use Tobu Tobu Girl as the selected action baseline (GB Pong remains the
   simpler directional control). Pause during active gameplay, hold Right or
   Right+A, then frame-advance until the player first changes. Count the first
   advance as 1. Restore the same slot before each trial; sample several game
   states and repeat for release. Record the individual counts and range.
   This checks frontend-visible frame response, not physical latency.
3. For a named fighting game, use a repeatable practice scene and compare the
   same action against a reference core with identical settings. Inspect a
   short tap, release, alternating directions, direction plus attack, and a
   repeatable command sequence. Count first visible movement separately from
   an attack's animation startup. Keep commercial content and captures private.
4. To measure physical latency, film the button actuation and display together
   at high frame rate, or use a switch/LED and optical sensor. At 240 fps,
   each camera frame spans about 4.17 ms; report the actuation definition and
   uncertainty. Collect at least 20 trials and report median and range;
   compare the same display region and setup against the reference core.
   Frame-advance results and timestamps from scripted input do not replace
   this measurement.
5. Play the selected action sequence for five minutes on each target device.
   Require reliable short taps/releases, continued control, no reproducible
   command loss and no sustained slowdown or audio underrun. Compare any
   reproducible extra frame against the reference setup before changing core
   timing. A user-observed comfortable result closes only that setup/game
   checkpoint; physical-latency claims require the measurement above.

If the baseline feels delayed, first locate the cost in the core/game response
or frontend/display path. Validate any setting change on that same setup.
Only then evaluate a bounded run-ahead configuration, checking determinism,
audio, short commands and performance as well as response time. Avoid a
blanket one-frame recommendation: games have different minimum internal lag,
and excessive run-ahead can cause visible rollback or other artifacts.

References: [Libretro latency controls](https://docs.libretro.com/guides/latency/)
and [run-ahead measurement and limitations](https://docs.libretro.com/guides/runahead/).
