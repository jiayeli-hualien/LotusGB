# PPU mid-scanline scrolling

## Current status

Mode 3 has separate 16-pixel BG and OBJ FIFOs. During OBJ arbitration, the BG
fetcher advances until it reaches Push or the BG FIFO has pixels queued, then
waits; a new BG row is pushed only when that FIFO is empty. A public PPU
regression checks that a SCY write during this pause affects bitplanes not yet
fetched. This catch-up predicate follows Pan Docs; SameBoy uses a different
fetcher-state boundary, so the regression records a hypothesis rather than
hardware-verified behavior.
The BG fetcher now exposes per-dot tile-map/bitplane address (T1) and data (T2)
phases, followed by separate FIFO-push and tile-column-advance phases. This
refactor preserves the current visible output and tested timing; OBJ events are
still coarse, precomputed stalls, so shared BG/OBJ bus arbitration is not yet
fully modeled. The attempted OBJ read-slot gate was removed after it proved
unobservable in the current renderer; OBJ plane reads remain separate coarse
actions, but the shared bus schedule is still unresolved. Across all 31
Mealybug PPU ROMs whose assembly mentions OBJ or sprites, the candidate and
prior `a7b3139` shade buffers were byte-identical.
The public SCY-during-OBJ regression also distinguishes the current catch-up
predicate from a stricter predicate-only alternative in a bounded 330-case
sweep. The selected `m3_scx_high_5_bits` image remains at 84 differing pixels
under both; neither result verifies hardware behavior.
`m3_scx_high_5_bits` remains at 84 differing pixels with its previous capture
hash, so this step does not claim better visual accuracy. `make test`, the
selected 11-ROM Mooneye PPU/interrupt run, and 3,600-frame Big2Small and
Canyon Racer callback smoke checks pass. The Canyon Racer frame differs from
the preceding run in 407 of 23,040 pixels; this profile has no expected frame,
so the comparison does not establish accuracy. Exact OBJ event alignment and
per-dot bus arbitration remain unverified.

A fresh rebuild of the current tree produced these selected Mealybug `DMG-blob`
pixel differences: BGP 3,642; SCX-low 540; SCX-high 84; SCY 8,334; BG-map
change 1,082; BG-enable change 2,210. All six remain **FAIL** against the
reference. The repeatable report is under
`build/test-roms/mealybug-vram-arbitration-candidate/report.json`; it records the
archive SHA-256
`e2fa6ca96ad48bd64d7297fda042db04c15117a730da3bbd590f139e89b150bd` and runner
SHA-256 `0fb0d7eab45a076278b8d39ef660b3aee738ea6aec9d432af5fa34cf0df4dacb`.
The linked `liblotusgb.so` SHA-256 for this run is
`976c3b15979988a4b934c949db9318762805fa518b46bd3144409a554957edf9`.
Earlier counts in the dated investigation sections are historical outputs
from prior builds; use this fresh run as the current image baseline. The new
report has the same six counts as the preceding baseline.

## Investigation by boundary (2026-10-26)

For the remaining SCX discrepancy, isolate one part of a scanline before
changing its timing. Passing Mode 2/Mode 3/Mode 0 boundaries constrains total
duration and interrupt/access edges; it does not establish the order of
fetches inside Mode 3. The 84-pixel image is an integration result, not a
reason to add or remove dots from an individual stage.

| Boundary | Existing evidence | What remains open / next action |
| --- | --- | --- |
| Mode 2 and entry to Mode 3 | `testVisibleLineModesAndVideoMemoryAccess` checks the 80-dot boundary; Mooneye checks selected STAT and OAM-access edges. Sprite selection tests cover the ten-object limit. | Selection is prepared at Mode 3 entry, rather than scanning OAM throughout Mode 2. The SCX ROM supplies stable OAM, so first isolate its Mode 3 behavior instead of rewriting OAM scanning. |
| CPU register writes | CPU load/store tests cover the M-cycle sequence; SystemClock tests check device advancement before the bus transaction. The current LY 8 trace places the SCX write at dot 114. | This is the current model's write time, not a hardware bus measurement. Keep the instruction flow fixed in control ROMs; a common reporting-marker cycle alone does not prove every register write has identical timing. |
| Mode 3, BG without OBJ | Public PPU tests cover startup, fine discard, tile-map sampling and separate SCY bitplanes. The new OBJ-disabled control matches SameBoy exactly. | This agreement applies to the selected SCX ROM, not every BG/palette effect. Preserve this control while investigating OBJ interaction. |
| Mode 3, OBJ wait/fetch/BG resume | One-object controls at OAM X=1 and X=8 each differ from SameBoy by six pixels on LY 8–14. | This is the next narrow investigation: compare BG address/data/push and OBJ completion around one object. Neither multiple objects nor left clipping is required to reproduce the discrepancy. Exact hardware arbitration is still unverified. |
| Mode 0 entry and line completion | The expanded 14-case Mooneye group passes, including sprite Mode 0 and SCX/HBlank timing. Public tests check several sprite penalties and 456-dot lines. | Preserve these endpoints. A correct endpoint can still contain incorrectly ordered internal fetches. Reopen an endpoint only when evidence identifies that edge. |
| Pixel composition and final image | `dmg-acid2` and the selected sprite-priority image pass; the SCX comparison localizes differences to one BG output tile. | Keep the original image as the final integration check. Do not treat a lower mismatch count as proof of a correct timing rule. |

### Controlled SCX ROM decomposition

`tools/diagnose_ppu_scx.py` derives four variants from the pinned upstream
`m3_scx_high_5_bits` ROM, leaving the instruction flow and cartridge header
unchanged. The OBJ-disabled control changes only the immediate LCDC value
from `$93` to `$91`. Single-object controls retain LCDC `$93`, hide the other
objects through their OAM Y data, and place one object on LY 8–15 at OAM X=1
or X=8. The source ROM hash and unique instruction/table patterns are checked
before patching; the diagnostic does not repair the global ROM checksum.

| Variant | LotusGB versus SameBoy differing pixels |
| --- | ---: |
| Original objects enabled | 84 |
| Objects disabled | 0 |
| One object, X=1 (left clipped) | 6 |
| One object, X=8 (fully visible) | 6 |

The single-object differences have the same coordinates: x=16, 17, 21 and 22
on a subset of LY 8–14. All four LotusGB captures reach the reporting marker
at PC `$064C` after 222,587 M-cycles. SameBoy's 120-frame and 180-frame outputs
are byte-identical for each variant, and its original-ROM output matches the
upstream expected PNG exactly, calibrating this comparison's startup/palette
settings. These are cross-core observations, not hardware validation; they
do not establish the correct fetch dot or rule out every CPU/PPU bus-phase
interaction.

Run this optional diagnostic with the existing Pillow venv and external ROMs:

```sh
make -C src/lib ppu-frame-runner build_dir="$PWD/build"
make -C src/libretro rom-test-runner build_dir="$PWD/build"
build/visual-venv/bin/python tools/diagnose_ppu_scx.py \
  --rom /path/to/m3_scx_high_5_bits.gb \
  --reference /path/to/expected/DMG-blob/m3_scx_high_5_bits.png \
  --comparison-core /path/to/sameboy_libretro.so
```

Reports, derived ROMs, logs, images and diff
maps go under `build/test-roms/ppu-scx-decomposition/`; binaries remain outside
version control. A completed diagnostic reports differences and is not a
conformance pass. Runner errors, unstable comparison frames, or failure to
reproduce the original reference cause a nonzero exit. This is not a CI job.

The fully visible X=8 control is the small reproducer for the event comparison
below. Preserve the OBJ-disabled control, both single-object controls,
original image and Mode 0/Mode 2 boundary checks when evaluating a candidate.

### Normalized CPU/PPU event comparison (2026-10-26)

An independent SameBoy build from the installed core's actual commit,
`c87d2565ea2eef1d342d0828c041ca71b5784aff` (1.0.4), reproduces the four
84/0/6/6 results. Instrumented builds of both emulators retain their baseline
frames. Align events to the **visible Mode 2 entry**, rather than comparing
their private line counters: SameBoy's `cycles_for_line` includes four dots
before that entry. PPU event timestamps account for its pending display-cycle
budget; CPU write timestamps are taken after synchronizing the PPU.

| Event on LY 8, single X=8 object | LotusGB, T-cycles from Mode 2 entry | SameBoy, T-cycles from Mode 2 entry |
| --- | ---: | ---: |
| Mode 2 STAT request | 0 | -1 |
| CPU accepts interrupt | 2 | 0 |
| CPU reads vector opcode | 22 | 20 |
| Visible Mode 3 entry | 80 | 80 |
| First visible pixel | 103 | 104 |
| SCX write of value 8 | 114 | 110 |
| Map address for BG output tile at x=16 | 113 | 112 |
| Map data for that tile | 114 | 113 |

Interrupt entry itself takes 20 T-cycles from acceptance to vector read in
both models. The SCX write is four dots later relative to Mode 2 in LotusGB,
while the disputed BG address sample is only one dot later. Thus the earlier
observation that the BG tile is sampled before the write does **not** establish
OBJ stall duration as the sole cause. CPU/PPU phase and register-write
visibility must be considered separately. SameBoy has an explicit
[DMG SCX write-conflict phase](https://github.com/LIJI32/SameBoy/blob/c87d2565ea2eef1d342d0828c041ca71b5784aff/Core/sm83_cpu.c#L302-L306),
two T-cycles earlier than its ordinary write handling. That is implementation
evidence, not an independent hardware measurement.

Two isolated candidates were checked without changing repository timing code:

| Candidate | Original / OBJ off / single X=1 / single X=8 differences | Expanded Mooneye PPU result | Decision |
| --- | --- | --- | --- |
| Mode 2 source one dot before visible entry | 84 / 0 / 6 / 6 | 14/14 | No change in the SCX write or images; not retained. |
| SCX CPU writes at T2 instead of T4, preserving four total T-cycles per step | 77 / 0 / 6 / 0 | 14/14 | Not retained: fixes 19 original differences but introduces 12 on rows 128–134. |

The other five selected Mealybug counts remain unchanged under the T2-write
candidate. Resolving the single X=8 case alone is insufficient evidence to
keep that policy. All production timing remains at the preceding baseline;
the original case still has 84 differences. Next, resolve the write-phase and
OBJ/BG scheduling contributions as separate contracts before implementing a
combined change. Require the original and all three controls, rather than a
lower total pixel count, to support the explanation.

Source-build calibration, logs, instrumentation patches and rejected-candidate
reports are under the ignored `build/test-roms/ppu-scx-source-calibration/`,
`ppu-event-traces/`, `ppu-mode2-source-candidate/`,
`ppu-scx-write-phase-candidate/`, and matching `mooneye-*` directories.
`diagnose_ppu_scx.py` now records the comparison core's identity through
`retro_get_system_info`; `.info` metadata is not used as its version source.

## Scope

The active route is one observable SCX/SCY fetch behavior at a time, with
selected independent visual ROMs and existing regressions. The first
checkpoint established a baseline; subsequent checkpoints add SCX coarse
sampling and separate SCY bitplane reads. Complete Mealybug conformance and a
full CGB FIFO implementation are not acceptance prerequisites; a narrow DMG
BG/OBJ fetch interaction may be needed for a measured case.

The current renderer groups scanline-local work in a Mode 3 state. BG pixels
pass through a 16-pixel FIFO, with separate tile-map and bitplane sampling
phases. OBJ pixels use a separate 16-pixel FIFO and are overlaid when a coarse
OBJ fetch event completes. Window pixels and OBJ descriptors are prepared at
Mode 3 entry; OBJ bitplanes are sampled at separate points during the coarse
fetch event. During an OBJ event, the current model stops BG catch-up at Push
or when the BG FIFO becomes non-empty; push waits for an empty FIFO. This is
the Pan Docs interpretation, while SameBoy waits for a different fetcher-state
boundary. Object timing remains a simplified, scanline-prepared schedule
rather than bus-driven BG/OBJ fetcher arbitration.

[Pan Docs scrolling](https://gbdev.io/pandocs/Scrolling.html) describes repeated
scroll-register sampling during tile fetch, with SCX's low three bits sampled
at scanline start. DMG also samples SCY separately for the tile bitplanes.
[Pan Docs Pixel FIFO](https://gbdev.io/pandocs/pixel_fifo.html) describes
separate BG/OBJ FIFOs and one OBJ-fetch catch-up sequence. Because SameBoy's
source uses a different wait boundary, neither description is enough to
settle this behavior without a hardware-backed observation. Use focused tests
for each observable transition rather than updating scroll coordinates
independently on every emitted pixel, which would bypass fetch latency and
cached tile data.

### Explicit BG fetch address/data phases (2026-10-25)

Replace the two-dot phase counter with per-dot states for tile-map address and
data, low-plane address and data, high-plane address and data, FIFO push, and
tile-column advance. T1 captures the VRAM offset; T2 reads VRAM. This exposes
the BG bus phases for later arbitration without changing the tested visible
output or current Mode 3 cadence. The OBJ schedule is still a list of
scanline-prepared coarse events; this does not implement shared per-dot bus
arbitration or establish a new hardware timing result.

`make test` and the selected 11-ROM Mooneye PPU/interrupt group pass. The six
selected Mealybug counts remain unchanged (3,642 / 540 / 84 / 8,334 / 1,082 /
2,210), and the six shade captures plus all 12 rendered/difference PNGs are
byte-identical to the preceding run. See the fresh report and build hashes in
[Current status](#current-status). The public PPU suite exercises fetch-visible
scroll, FIFO, pixel, and line boundaries; it does not inspect these private
state names directly.

### Timed OBJ VRAM slot (2026-10-25)

The existing OBJ fetcher already advances once per dot through alignment,
setup, and transfer stages, with low/high plane reads at separate boundaries.
Route those read actions through one arbitration point: an OBJ plane read uses
the modeled VRAM slot for that dot, so the BG catch-up fetch does not also
advance; on other OBJ-fetch dots it keeps the existing Push/FIFO predicate.
This avoids scheduling two PPU VRAM reads on one modeled dot. The policy is a
working DMG bus model, not a hardware-verified timing result.

`make test` and the selected 11-ROM Mooneye PPU/interrupt group pass. All six
selected Mealybug shade captures and their rendered/difference PNGs are
byte-identical to the previous build. The selected ROMs therefore do not
exercise a case that distinguishes this policy; a hardware-backed or otherwise
independent observable test is still needed before claiming improved accuracy.
The fresh report is `build/test-roms/mealybug-vram-arbitration-candidate/report.json`.

SameBoy is a useful independent implementation for comparing fetcher state and
ordering, but it is not hardware ground truth. A LotusGB result can be more
accurate for a particular behavior only when an independent hardware-backed
test or observation supports it; there is no evidence here that LotusGB is
more accurate overall.

## External visual baseline

Source: [Mealybug Tearoom Tests](https://github.com/mattcurrie/mealybug-tearoom-tests),
checkout `70e88fb90b59d19dfbb9c3ac36c64105202bb1f4`.
The source repository includes a prebuilt ROM archive and expected screenshots.
The `DMG-blob` screenshots are emulator reference renders; the project also
provides hardware photographs. Do not describe the reference PNGs as exact
raw hardware captures. Sources and images remain in the external checkout.

```sh
python3 -m venv build/visual-venv
build/visual-venv/bin/python -m pip install -r requirements-visual-test.txt
make -C src/lib ppu-frame-runner build_dir="$PWD/build"
build/visual-venv/bin/python tools/run_mealybug_tests.py \
  --suite-dir /path/to/mealybug-tearoom-tests
```

If `python3 -m venv` reports that `ensurepip` is unavailable, install the
distribution's venv package first (on Ubuntu, `sudo apt install python3.10-venv`).
Pillow is only required for this optional image-comparison workflow; it is not
part of the core build or required CI.

The runner captures 160×144 shade indices at the fixed-ROM0 `LD B,B` reporting
marker. Its PC convention matches the CPU's next-opcode prefetch. This is
scoped to the selected ROMs; bank-switched/RAM reporting code is unsupported.
A capture marker means an image is ready, not that the test passed. Timeout,
CPU failure, STOP and output errors fail capture. The Python comparison maps
shade indices to the documented `$FF/$AA/$55/$00` grayscale values and returns
failure for any differing pixel.

`--test m3_scx_high_5_bits` selects one case. The default set also includes
`m3_lcdc_bg_map_change` and `m3_lcdc_bg_en_change`, which toggle LCDC's
background-map and BG enable bits during Mode 3. Outputs default to
`build/test-roms/mealybug/`: ROM copies, shade buffers, PNGs, red mismatch maps,
logs and `report.json`. The report hashes the archive, each selected ROM and
reference image, and runner executable. The output directory must remain
outside the reference checkout. This optional external suite is not in CI;
`make test` includes ROM-free capture-protocol probes instead.

### Mooneye manual sprite-priority image (2026-10-25)

The Mooneye `manual-only/sprite_priority` ROM is a screenshot-based check; the
Mooneye test documentation marks `manual-only` tests for manual verification
on real hardware. The c-sp v7.0 test-ROM bundle provides a DMG expected image
and specifies the common grayscale palette. LotusGB's captured DMG frame
matches that image exactly (0 of 23,040 differing pixels). This validates the
sprite-priority composition in this scene; it does not check the BG/OBJ fetch
catch-up boundary or prove exact per-dot sprite timing.

Run it with `make test-mooneye-visual`. The ROM and reference image must be in
the external c-sp bundle at `mooneye-test-suite/manual-only/`; output images,
runner log, and hashes are written under
`build/test-roms/mooneye-sprite-priority/`. The ROM and image are not
committed. Sources: [Mooneye test-suite structure and manual-only checks](https://github.com/Gekkio/mooneye-test-suite#suite-structure),
[c-sp screenshot procedure and palette](https://github.com/c-sp/game-boy-test-roms/blob/master/src/howto/mooneye-test-suite.md#screenshot-based-tests),
and the [v7.0 bundle](https://github.com/c-sp/game-boy-test-roms/releases/tag/v7.0).

### Initial result (2026-10-24)

| Test | Different pixels / 23,040 | Result |
| --- | ---: | --- |
| `m3_bgp_change` | 3,642 | FAIL |
| `m3_scx_low_3_bits` | 540 | FAIL |
| `m3_scx_high_5_bits` | 6,426 | FAIL |
| `m3_scy_change` | 9,873 | FAIL |

All four reached their reporting marker. The high-SCX actual/reference images
were inspected; the current output lacks the reference's changed tile columns.
BGP is retained as an output-timing diagnostic, not a passing control.
No PPU behavior was modified to obtain these results. They are newly measured
coverage gaps, not evidence that this tooling change broke emulation.

## Initial scroll-sampling checkpoint (completed)

1. Add an explicit background tile-fetch/cache boundary for SCX high-bit
   sampling while preserving scanline-start fine-scroll discard.
2. Add public PPU-interface tests for already-fetched versus later tiles and
   unchanged low-bit behavior.
3. Re-run selected external images and record exact remaining differences;
   avoid hiding a visual mismatch with an unsupported global delay.
4. Retain dmg-acid2, unit tests, selected Mooneye regressions and homebrew
   profiles. Keep SCY and sprite/window interactions as separate steps.

## SCX coarse sampling implementation

The first renderer step introduces a background tile-cache update on the
existing simplified fetch clock. It samples SCX bits 3–7 for each subsequent
8-pixel tile and retains the scanline-start SCX bits 0–2 for initial discard.
Already-emitted pixels and already-fetched tiles are preserved. SCY remains
latched for the line at this step; individual bitplane reads, exact FIFO
occupancy and sprite/fetcher arbitration remain unresolved timing work.

Object selection still happens at Mode 3 entry, but behind-BG priority is now
applied when each pixel is emitted, against the actual fetched background
color. Window pixels retain their existing path. This avoids deciding object
visibility from a stale background snapshot after SCX changes.

Focused public-interface tests cover SCX writes after a tile fetch, retained
fine discard for initial offsets 0 and 3, and behind-BG object visibility over
newly fetched color-zero background. The old implementation fails the new
coarse-scroll test. Exact external visual acceptance is tracked separately;
this cache clock is not a claim of a full hardware fetcher/FIFO implementation.

### Gameplay target

[Canyon Racer](https://bbbbbr.itch.io/canyon-racer) is a candidate for later
interactive validation. Its [source description](https://github.com/bbbbbr/canyon-racer#about-the-effects)
explicitly uses mid-scanline SCY writes for vertical parallax, alongside X
wave scrolling. Use the official Game Boy MBC5 build, keep its mixed-license
ROM/assets external, and record the selected release/hash when supplied.
The first gameplay check is recorded in [the homebrew procedure](homebrew-check.md#canyon-racer-raster-effect-gameplay-check); exact SCY effect accuracy remains unverified.

### Measured result after coarse SCX sampling

`m3_scx_high_5_bits` decreases from 6,426 to **84 differing pixels**, still
FAIL. Remaining differences are confined to x=16–23 on selected rows, so
fetch/sprite interactions need further investigation rather than assuming
complete timing accuracy. BGP (3,642), SCX-low (540) and SCY (9,873) remain
unchanged. The source of those differences has not been established by this
checkpoint.

Validation at this checkpoint: `make test` passes, including dmg-acid2 and
both protocol test tools. The default combined Mooneye set remains 67/67;
GBIT reports 244/244 ordinary and 256/256 CB opcodes. Big2Small and Tobu Tobu
Girl both complete 3,600 frames with their preceding final frame hashes;
the supplied Big2Small player SRAM remains unchanged. The rebuilt libretro
`.so` includes the change. These regressions supplement, rather than replace,
the still-failing Mealybug timing measurements above.

## Separate SCY bitplane reads

Background fetches now latch a tile number, read the low bitplane two dots
later and the high bitplane after another two dots, then commit its eight
pixels to the cache. SCY is sampled for the tile-map row and independently
for each bitplane's row offset. The tile number remains latched if SCY crosses
a tile-map row between stages. SCX coarse sampling keeps its prior phase.
This follows the DMG register sampling behavior described in
[Pan Docs scrolling](https://gbdev.io/pandocs/Scrolling.html) and the separate
[fetch stages](https://gbdev.io/pandocs/pixel_fifo.html).

New public PPU tests write SCY before, between and after the bitplane reads,
checking new-row data, mixed bitplanes and already-read data respectively.
Another test crosses a tile-map row after the map read and verifies that only
the next tile number changes. The preceding implementation fails the new
SCY regression. These are sampling tests on the current clock baseline, not
independent proof of every T-cycle in sprite/FIFO arbitration.

### Bounded investigation of the remaining SCX pixels

The saved instrumented trace predates the BG catch-up condition correction.
In that trace, started after the ROM writes LCDC=`0x93`, line 8's third
tile-map address is sampled at dot 111 and the SCX write at dot 114; line 64
samples that address at dot 112, again before the write at dot 114. The modeled
initial OBJ stalls are 9 and 10 dots on those lines. Earlier temporary traces
captured the pre-setup post-boot frame or an older source copy and are not
valid timing evidence. Testing
coarse-sampling phases 6, 7, 8, 9 and 10 yielded 84, 84, 77, 80 and 45
SCX-high differing pixels. None reached an exact match, and the lower mismatch
count alone does not establish hardware timing. Those variants were
discarded; the existing phase remains.

The current capture has all 84 differences inside x=16–23, on lines 8–22 and
64–86. The test writes `SCX=LY` during Mode 3 and places one sprite on each
eight-line band; for example, line 8 uses OAM X=1 and line 64 uses OAM X=8.
These observations narrow the failure to a single output tile column on
sprite-affected lines, but do not by themselves identify the correct fetch
dot. [Pan Docs](https://gbdev.io/pandocs/Scrolling.html) specifies that SCX's
coarse bits are reread on each tile fetch, but does not specify this exact
sprite/fetcher boundary. SameBoy's [fetcher address stage](https://github.com/LIJI32/SameBoy/blob/master/Core/display.c#L895-L908)
calculates the map address at `GET_TILE_T1` from live SCX and the current
position in the line. That is a useful implementation comparison, not
independent hardware evidence.

The pre-correction model's stage trace around the affected tile was:

| LY | First OBJ output stall | Tile-column 2 map / low / high / push | SCX write |
| ---: | ---: | --- | ---: |
| 8 | dots 93–101 (9 dots) | 111 / 113 / 115 / 117 | dot 114 |
| 64 | dots 93–102 (10 dots) | 112 / 114 / 116 / 118 | dot 114, after that dot's low-plane read |

That pre-correction model latched the tile-map address before the write on both
lines. On line 8, the write landed between the low- and high-plane reads; on
line 64, it followed the low-plane read. This matched that version's stage
ordering, but the reference image differed in the output tile at x=16–23. The
candidate error is the modeled map-fetch position around OBJ arbitration, not
whether later tile fetches reread SCX. This is still a comparison against the
Mealybug emulator reference image, not a hardware capture. This table and the
following probe describe the pre-correction model; the current-build LY 8
trace is recorded in [the later timing note](#localizing-the-remaining-scx-mismatch-2026-10-25).

A temporary public-interface probe isolated line 8 with one object at OAM
Y=24/X=1 and changed SCX at dot 114. The pre-correction model still used
tile-map column 2 for pixel x=16, while the Mealybug DMG reference shows the
tile shifted by the new SCX value. The probe failed against that model and was
removed rather than keeping a speculative expected value in the normal unit
suite. The external ROM comparison remains the reproducible target for this
case. A fresh capture from the corrected `ppu_frame_runner` still differs in
84 pixels and has shade-buffer SHA-256
`0655c10657494025e66e223502bb9ef6aa5f0c0378cce02386ae6a7a382cf805`.

The likely modeling gap is that LotusGB still schedules each object as a
precomputed output stall. BG and OBJ pixels pass through separate FIFOs, and
OBJ bitplanes are sampled at two coarse event boundaries, but the event's
duration and descriptor are still prepared at scanline start. The catch-up
rule now stops when the fetcher reaches Push or the BG FIFO becomes non-empty;
it does not model per-dot OAM/VRAM bus access or the full sprite sequence. An
earlier candidate that pushed a BG row into a non-empty FIFO increased the
Mealybug mismatch from 84 to 150 pixels (x=16–31, y=8–142). Requiring an empty
FIFO before pushing a row restored the previous 84-pixel capture hash. This
supports the FIFO condition in the model but does not prove exact hardware
timing.
Pan Docs describes separate BG and OBJ FIFOs, and says sprite arbitration
advances the BG fetcher until it reaches step 5 or the BG FIFO becomes
non-empty ([Pixel FIFO](https://gbdev.io/pandocs/pixel_fifo.html)). SameBoy's
[OBJ-fetch path](https://github.com/LIJI32/SameBoy/blob/master/Core/display.c#L1876-L1901)
uses a different wait predicate: its loop continues while the fetcher is
before `GET_TILE_DATA_HIGH_T2` **or** the BG FIFO is empty, so it exits only
after both conditions are satisfied. It then advances the fetcher two more
times. The enum is declared
[here](https://github.com/LIJI32/SameBoy/blob/master/Core/display.c#L824-L834).
These descriptions do not establish which sequence matches DMG hardware.
SameBoy is an implementation comparison, not a hardware oracle, and the new
SCY regression currently encodes the Pan Docs interpretation rather than an
independently verified hardware result. Keep the two predicates as an open
question until a targeted hardware-backed test can distinguish them.

The public PPU regression
`testCoarseScrollWrittenDuringObjectFetchAffectsLaterTiles` changes SCX at
line 1 dot 95, during the first OBJ stall, and checks that later tile columns
use the updated coarse offset. The regression
`testObjectFetchStopsBackgroundCatchUpWhenBgFifoHasPixels` places a transparent
object at screen x=3 and changes SCY during its fetch. It fails with the prior
conjunction (waiting for both Push and a non-empty FIFO) and passes when
catch-up stops on either condition. This is a regression for the selected
Pan Docs interpretation; it does not establish which predicate matches DMG
hardware, because SameBoy's current source uses a different wait sequence.
Keep line timing, STAT, palette-write, and pixel regressions as constraints.
Use a targeted hardware-backed result to resolve this question before treating
either implementation as the reference. Do not tune the global phase to chase
a lower pixel count. Keep CGB FIFO behavior and unrelated fetch quirks out of
scope.

### Mode 3 state and BG FIFO foundation (2026-10-25)

Group the private per-line pixel buffers, fetch registers, output position and
stall counters in `DmgPpu::Mode3State`, and move per-dot work into
`advanceMode3Pipeline()`. The BG fetcher now pushes raw color indices into a
16-pixel ring FIFO, and visible output pops pixels from it. Fine SCX discard
also removes pixels from this queue. Window pixels still use their line-start
cache. OBJ stalls are represented as precomputed events with `AlignmentWait`,
`FetchSetup` and `DataTransfer` stages, but these coarse phases preserve the
old total duration and do not model individual bus reads; sprite interaction
is not hardware-complete.

`make test` passes, including the framebuffer and line/STAT regressions. A
direct `m3_scx_high_5_bits` capture still stops at PC `$064C` (decimal 1612); comparing the
shade buffer against the external DMG reference gives 84 differing pixels in
x=16–23, y=8–86, unchanged from the prior model. The repository comparison
wrapper requires Pillow, which is unavailable here; a small standard-library
PNG decoder was used to count the reference differences.

An experiment advanced the background fetcher on every dot of the existing
precomputed OBJ stall. It broke
`testCoarseScrollWrittenDuringObjectFetchAffectsLaterTiles` and increased the
Mealybug mismatch from 84 to 342 pixels, so it was reverted. Advancing BG
fetch only during `AlignmentWait` broke the same regression but produced an
identical Mealybug frame hash. Advancing during the four-dot fetch-setup phase
passed the focused PPU test but raised the mismatch to 256 pixels; advancing
only during the later data-transfer phase raised it to 289. All variants were
reverted. These broad phase choices do not identify the correct dot-level
arbitration. The current OBJ event state machine separates alignment, fetch
setup and data-transfer time without changing output; next, model individual
OAM/VRAM fetch actions and test each behavior change against existing PPU
behavior before using the external image as a diagnostic.

### Measured SCY result

The SCY diagnostic decreases from 9,873 to **8,330 differing pixels**, still
FAIL. SCX-high remains 84; BGP and SCX-low stay at 3,642 and 540. `make test`,
67/67 default Mooneye cases and GBIT (244/244 ordinary + 256/256 CB) pass.
Both 3,600-frame homebrew profiles preserve their prior final frame hashes
and the original supplied SRAM. The rebuilt core includes these stages, but
full raster-effect compatibility has not been established.

## Gameplay acceptance boundary

The current work targets advanced visible raster effects beyond the selected
games already checked. Common-game smoke success does not establish broad
compatibility, and remaining synthetic pixel differences are diagnostic
evidence rather than a requirement to implement every obscure PPU behavior.
Use Canyon Racer's visible SCY parallax as the next gameplay check, retaining
Mealybug images to explain deviations. The official ROM is distributed on
the author's itch.io page; the GitHub release currently has no ROM asset.
The official 0.7.7 standard MBC5 ROM is now supplied and has completed a
scripted gameplay check. See the homebrew procedure for its hash, input
sequence and limits; no complete compatibility result is claimed.

## First Canyon Racer gameplay observation

The standard 0.7.7 MBC5 ROM reaches gameplay with visible ship, obstacles and
HUD during the 3,600-frame scripted run. Compared with `c13d0f0` at frame 1800,
the SCY-stage version changes 3,463 RGB pixels, with the same input and
startup profile. The canyon textures/rows visibly change between versions.
This provides a real-game target for the sampling work alongside the still
failing Mealybug checks; further accuracy claims require a controlled hardware
or reference comparison. [Procedure and artifacts](homebrew-check.md#canyon-racer-raster-effect-gameplay-check).

## Mid-scanline background tile-map select

The first background renderer latched LCDC bit 3 when Mode 3 began, so a
mid-scanline map change could not affect any later background tile. The DMG
Mealybug `m3_lcdc_bg_map_change` test toggles this bit during Mode 3 while
sprites create different fetch delays. The renderer now samples the selected
map at each background tile-map fetch. A public PPU test writes different
tiles into both maps, changes LCDC after the first tile is fetched, and checks
that the first tile stays unchanged while the next tile uses the new map.

Against the selected `DMG-blob` reference image, the difference count changes
from 1,234 to 1,082 pixels, so this case remains **FAIL**. The reference is an
author-provided emulator render, not a raw hardware capture. The remaining
sprite-dependent timing differences are not resolved by shifting the map-read
phase globally; exact FIFO/fetch arbitration needs separate evidence.

## Mid-scanline LCDC.0 background enable

On DMG, clearing LCDC.0 blanks the background/window while leaving objects
visible. LotusGB now checks this bit at each pixel output and uses the live
value for object-behind-background priority. Public PPU tests cover pixels
emitted while the bit is clear and an object marked “behind BG” becoming
visible when the BG priority layer is disabled. This follows the
[Pan Docs LCDC.0 description](https://github.com/gbdev/pandocs/blob/master/src/LCDC.md#lcdc0--bg-and-window-enablepriority).

The Mealybug `m3_lcdc_bg_en_change` comparison decreases from 2,189 to 1,750
differing pixels, but remains **FAIL** against its emulator reference image.
The current change covers output and priority selection; it does not model a
hardware fetcher/FIFO reset when LCDC.0 changes mid-line.

### Re-enabling BG after it starts disabled

The PPU previously latched LCDC.0 at Mode 3 entry to decide whether the
background fetch clock would run. If BG was disabled as the scanline began,
turning it on later could not restore output because no tile data had been
fetched. LotusGB now keeps the background fetch clock running independently
of the LCDC.0 output gate. A public PPU test starts the line with BG disabled,
keeps the first three output pixels blank, then enables it and checks that the
already fetched tile becomes visible. [Pan Docs](https://github.com/gbdev/pandocs/blob/master/src/LCDC.md#lcdc0--bg-and-window-enablepriority)
documents the DMG-visible result and that LCDC may be changed mid-scanline.
This implementation follows the separation between BG fetching and per-pixel
LCDC.0 composition in the [SameBoy renderer](https://github.com/LIJI32/SameBoy/blob/master/Core/display.c#L3273-L3399).

This does not explain the remaining Mealybug mismatch: that test starts with
BG enabled and varies sprite positions to shift Mode 3 timing. Its 1,750
differing pixels remain a separate fetch/sprite timing question.

## Mid-scanline LCDC.1 object enable

LCDC.1 gates object display and can be changed during a frame. LotusGB now
retains the selected object descriptors even if LCDC.1 is clear when Mode 3
starts, reads their tile planes at the coarse fetch event, then checks the live
bit as each pixel is emitted. It only adds the simplified object-fetch
penalties when LCDC.1 was enabled at scanline preparation; timing when LCDC.1
changes during an active fetch remains unmodeled. Focused PPU tests cover
hiding and restoring upcoming object pixels, enabling objects after scanline
preparation, and preserving baseline Mode 3 timing while objects remain
disabled. This follows the basic register
semantics in [Pan Docs](https://eldred.fr/pandocs/LCDC.html#lcdc1--obj-enable)
and the separation of OAM scan from the per-pixel LCDC.1 display gate in the
[SameBoy renderer](https://github.com/LIJI32/SameBoy/blob/master/Core/display.c#L3202-L3318).

The Mealybug `m3_lcdc_obj_en_change` image remains 146 pixels different and
did not change with this implementation. Its [test source](https://github.com/mattcurrie/mealybug-tearoom-tests/blob/master/src/ppu/m3_lcdc_obj_en_change.asm)
uses object tile `$19` as the Nintendo logo pattern without loading that tile;
the ROM expects the pattern left in VRAM by boot-ROM execution. LotusGB's
post-boot test profile skips the boot ROM and starts VRAM empty, so those
pixels do not establish a remaining LCDC.1 mismatch. The Mealybug project
describes its expected images as its own emulator renders, not raw hardware
captures; use them as diagnostics only.

## OBJ FIFO staging and BG catch-up (2026-10-25)

Add a separate 16-pixel OBJ FIFO. Each selected object's descriptor (tile,
row, and attributes) is captured at scanline preparation. The low tile plane
is read after the coarse fetch setup and the high plane at event completion;
then the row is decoded and overlaid into the OBJ FIFO. Transparent pixels
leave higher-priority OBJ pixels intact, and the FIFO shifts only when a
visible screen pixel is emitted. Partly clipped objects start overlaying at
their first visible source pixel. If OBJ output is disabled at scanline
preparation, zero-stall events read and queue the selected object at its screen
position, retaining the ability to reveal upcoming pixels after LCDC.1 is
enabled mid-line.

The existing alignment/setup/data-transfer durations are retained. During a
timed OBJ event, BG fetching advances until it reaches Push or the BG FIFO has
pixels queued, then waits; BG rows push only into an empty FIFO. A public
regression changes SCY during the pause and confirms that unread bitplanes
sample the new row. The test failed under the prior conjunction condition and
now passes. `make test` and the selected 11-ROM Mooneye PPU/interrupt run pass; the
Big2Small and Canyon Racer callbacks each completed 3,600 frames. The direct
`m3_scx_high_5_bits` capture remains at 84 differing pixels with the previous
shade-buffer hash. A permissive FIFO push candidate increased the mismatch to
150 pixels and was discarded. The visual comparison is diagnostic, not a
hardware oracle. Canyon Racer's captured frame differs from the preceding
baseline in 407 of 23,040 pixels; there is no expected frame for this smoke
profile, so this is recorded as a comparison, not judged as an accuracy
improvement or regression. Exact event dot positions and per-dot bus behavior
still need independent evidence; next investigate a specific sprite/fetch
boundary without changing the established total stall schedule.

## Cross-core comparison for the remaining SCX pixels (2026-10-25)

The Mealybug `m3_scx_high_5_bits` ROM was run through the locally installed
SameBoy libretro core (1.0.4, `c87d256`) and Gambatte libretro core (v0.5.0) using
headless frontends based on `libretro_rom_runner`, with no input. Each core's
final image was identical across runs of 120, 180, and 240 frames. SameBoy's
120-frame RGB image matches the Mealybug `DMG-blob` reference pixel-for-pixel.
Gambatte uses
RGB565, so its three output colors were normalized to the corresponding
black/gray/white reference levels; its image also matches pixel-for-pixel
after that palette normalization. LotusGB's capture still differs from the
reference in 84 pixels, all within x=16–23 and y=8–86. Agreement between two
cores and the Mealybug reference raises confidence that this is a real LotusGB
output discrepancy, but it does not establish which behavior matches physical
DMG hardware.

The SameBoy version was corrected on 2026-10-26 by calling its
`retro_get_system_info` entry point. The installed `.info` file says 0.14.7
and is stale; the binary and frame hashes recorded below are unchanged.

The stock `libretro_rom_runner` rejected Gambatte because it does not advertise
`RETRO_ENVIRONMENT_GET_CAN_DUPE` or accept RGB565. A temporary runner under
`/tmp` added those frontend capabilities; no repository code was changed for
the comparison. The local artifacts are under the ignored `build/test-roms/`
directory. The ROM SHA-256 is
`423db91af0f87547ae46690044ef760481f7005033ffc61bd69875b7df96139b`; the
reference PNG SHA-256 is
`21750dfddb16dbbc084032f5361a82782685441eaca93bf67249049bb14b3a1b`. The
SameBoy core SHA-256 is
`5f3cbc610d1de2c04d41319b6678bece8c5bc78b1067436e1f6d9d440b89b56e`, and its
stable RGB frame SHA-256 is
`7c7e883096c6d23ade3e8347e333c5f2c14c7c34a1e581d2cd0dc2e406e74845`. The
Gambatte core SHA-256 is
`6fc0b2119b9ddfcf705566a720cf823f1834bb5726de45052a468af324de1463`, and its
stable RGB frame SHA-256 is
`d0ee4da13fc2fc48b8fc340f499f33d86089fb5abfec1a307f00f2d5569a36d9`. The
SameBoy run attempted to load `./dmg_boot.bin`, which was unavailable to the
runner; Gambatte reported no system directory and loaded the plain ROM. These
are local boot-path limitations. Mealybug's expected image remains an emulator
reference, not a hardware capture; use this result to localize the discrepancy,
not to copy either core's fetch rule without further evidence.

I checked the Mealybug checkout at `70e88fb`: `photos/DMG-blob` has no
`m3_scx_high_5_bits` photo. The corresponding photos are only for CPU CGB C
and CPU CGB D, so they cannot verify this DMG result. The project's
[README](https://github.com/mattcurrie/mealybug-tearoom-tests#usage) says its
expected screenshots come from the author's emulator and describes the
real-device photos as blurry. The SameBoy, Gambatte and Mealybug agreement is
therefore strong cross-implementation evidence, but these results do not
independently establish the physical DMG behavior. Keep the reference image as
a provisional target; do not copy a core's timing rule solely to match it.

SameBoy and Gambatte are separate codebases with different PPU implementations:
[SameBoy's display loop](https://github.com/LIJI32/SameBoy/blob/master/Core/display.c)
advances a fetcher state machine during display ticks, while
[Gambatte's PPU](https://github.com/libretro/gambatte-libretro/blob/master/libgambatte/src/video/ppu.cpp)
uses timed PPU state callbacks. Their agreement is useful cross-checking, but
it cannot rule out a shared interpretation error or an error in the emulator
reference image. Keep their frames as provisional comparison targets; hardware
evidence remains the authority for deciding a disputed dot-level behavior.

### mGBA third-core comparison

The installed mGBA libretro core (0.10-dev) produced a stable image at 10, 11,
12, 120, 180, and 240 frames, but its 120-frame image differs from the Mealybug
`DMG-blob` reference in 8,300 pixels after mapping its three RGB565 output
levels (black, mid-gray, white) to reference shades 0, 2, and 3. The differing
pixels span x=16–159 across most visible rows. SameBoy and Gambatte still match
the reference exactly under the same shade mapping.

The headless runner explicitly selected `mgba_gb_model=Game Boy` and disabled
`mgba_use_bios` and `mgba_skip_bios`. Its automatically selected model produced
the same frame. The ROM header is DMG-only (`$143 = $00`). The core SHA-256 is
`c840f305825d07c9bec7a6ec18841073c4b539fd234f73623474f97c507f1cdc`; the
stable 120-frame RGB capture SHA-256 is
`d2ab6bcd40d5397f69a4698cb4ddc277ebc68d9dcd010f58222f559c8fb7c21e`.

This third output shows that emulator agreement is not unanimous. It does not
identify which image matches physical DMG hardware: the Mealybug reference is
not a hardware capture, and the different output may reflect mGBA's behavior or
an unresolved difference in test execution. Keep the mGBA result as a recorded
cross-check, not as a reason to change LotusGB's timing model by itself.

## Timed OBJ/BG VRAM-read candidate scan and follow-up (2026-10-25)

To check whether an existing visual ROM distinguishes the timed OBJ read-slot
candidate, I built `ppu_frame_runner` from the preceding `a7b3139` revision in
`/tmp` and compared its shade buffers with the `d4e1254` candidate. The scan
covered all 31 Mealybug PPU assembly files mentioning `OBJ` or `sprite`; all
candidate and prior buffers were byte-identical. The two inspected cases,
`m3_bgp_change_sprites` and `m3_wx_4_change_sprites`, also produced identical
buffers across revisions.

I then swept public PPU writes over sprite X positions 1–167 and write dots
80–160, once changing SCY between distinct tile rows and once switching the BG
tile map between distinct patterns. Each 13,527-case sweep produced the same
framebuffer with and without the read-slot gate. Instrumentation found gate
overlaps only after a window had started in the tested scenarios; the current
renderer emits window pixels from line-prepared data, so those BG catch-up
steps cannot affect visible window pixels or Mode 3 duration. The gate was
removed as unobservable in the current model. This does not prove that hardware
allows simultaneous BG/OBJ VRAM reads; implementing shared arbitration should
wait until the renderer has an observable path and a test that distinguishes
the competing schedules.

These results show only that this candidate does not change the current
Mealybug outputs. They do not confirm shared-bus arbitration. The BGP sprite
case uses boot-ROM tile `$19` without loading it, so its image is not a valid
visual oracle for LotusGB's skipped-boot test profile. The WX sprite case
combines window reactivation with object priority; its expected PNG is the
Mealybug author's emulator output, while the accompanying real-device photo is
too blurry to identify individual fetch dots. Do not change OBJ durations or
BG catch-up rules based on these comparisons; a discriminating DMG observation
or a purpose-built case with a separate hardware oracle is still needed.

## Global CPU bus-phase experiment (2026-10-25)

Moving every CPU bus transaction earlier is not a viable fix for this image.
I built temporary copies with the peripheral advance set to 1, 2, or 3 T-cycles
before each bus transaction, then captured `m3_scx_high_5_bits` at the same
`LD B,B` marker. The current 4-T-cycle phase has 84 differing pixels in rows
8–22 and 64–86. The 3-T-cycle phase has the same count and affected row bands.
The 1- and 2-T-cycle phases reduce the count to 45, but move the differences to
rows 128–142; that is a displaced error, not an improvement in total behavior.

Each early-phase variant also fails the existing
`testSystemClockAdvancesDevicesToCpuBusPhaseBeforeTransaction` regression,
which requires clocked components to reach the M-cycle boundary before the CPU
transaction. The current 4-T-cycle build passes the full unit-test suite.
These global phase variants were isolated in `/tmp` and were not kept in the
repository. This rules out changing the system-wide bus phase as a general fix;
it does not settle any narrower PPU register timing detail. Keep the clock
phase and continue investigating the OBJ/BG fetch ordering within Mode 3.

## Localizing the remaining SCX mismatch (2026-10-25)

The differing pixels come from BG tile-fetch timing, not layer composition.
A fresh trace of the current build confirms the ordering on LY 8: the fetcher
computes tile-column 2's map address at dot 112 with `SCX=0`, reads that map
entry at dot 113, and the CPU's `SCX=8` write reaches the PPU at dot 114. At
the write, the fetcher has completed the low-plane address phase and is in its
low-plane data phase. The OBJ fetcher is idle by then; its preceding stall has
shifted the BG schedule. The later tile-column 3 map address is computed at
dot 120 using `SCX=8`.
[Pan Docs](https://gbdev.io/pandocs/Scrolling.html) states that SCX is sampled
again for each tile fetch, matching this mechanism.

The trace used compile-time instrumentation in a separate debug build, which
was then removed. Its captured shade buffer has the same SHA-256 as the normal
runner's prior capture, and the visual result remains at 84 differing pixels.
This establishes the current model's CPU-to-PPU event order at its M-cycle
boundary; it does not establish the physical CPU's exact bus phase within that
M-cycle. It narrows the modeled mismatch to the earlier OBJ stall and following
BG fetch schedule without proving which hardware timing should replace it.

A temporary, line-scoped timing probe supports the diagnosis: adding 3 dots to
the LY 8 OBJ event moves its column-2 map read to dot 115, after the SCX write,
and removes that row's pixel difference. Adding 4 dots on LY 16 and 2 dots on
LY 64 likewise removes the differences on those rows. The temporary probe code
was restored; its captures remain under the ignored `build/test-roms/` tree.

This localizes the model error to the relative timing of OBJ-induced Mode 3
stalls and subsequent BG tile fetches. LotusGB currently schedules OBJ stalls
as precomputed alignment/setup/transfer events; exact per-dot BG/OBJ arbitration
and same-dot CPU/PPU ordering remain unresolved. The different per-line probe
values are diagnostic only: applying a global delay changed other rows and is
not a valid fix. The cross-core agreement raises confidence in the expected
pixels, but hardware behavior has not yet been independently confirmed.

### Isolating the left-edge OBJ boundary

A standalone PPU probe gives the same boundary in a smaller setup. It writes
SCX from 0 to 8 at different dots on line 1, samples pixel x=16, and places a
transparent OBJ at different OAM X coordinates. Without an OBJ, the write
boundary is dot 103. With OAM X=0, 1 and 2, it moves to dots 113, 112 and 111;
with OAM X=8, 9 and 10, it moves to dots 113, 112 and 111. These are measured
write boundaries in LotusGB's current model, not hardware timings. They show
that the prepared OBJ event's start position and length jointly set when the
next tile becomes insensitive to SCX.

An isolated candidate changed the first-event overlap, clamped the alignment
coordinate for left-clipped OBJs, and added one dot. It clears the existing
differences on lines 8–22 and 64–70, but the full image regresses from 84 to
159 differing pixels: new differences appear on lines 24–62 and 128–134, while
lines 72–86 still differ. The candidate was discarded. This rules out that
simple coordinate clamp and global first-event adjustment as a complete fix;
it does not rule out a narrower left-edge behavior.

An isolated catch-up candidate that advanced BG fetch while the FIFO was
non-empty until the fetcher reached Push left the Mealybug result unchanged at
84 differing pixels. It also failed the existing public PPU regression that
writes SCY during an OBJ fetch and checks the unread BG bitplanes. This broad
catch-up change was discarded; the test is evidence that the BG wait rule
cannot be changed independently of the bitplane sampling behavior.

A second, short-lived candidate used a SameBoy-inspired stop condition (high-data
T2 plus a non-empty BG FIFO), followed by two fetcher advances in LotusGB's
coarse scheduler. It failed `testCoarseScrollWrittenDuringObjectFetchAffectsLaterTiles`
and was reverted. This does not test SameBoy's timing faithfully: LotusGB does
not yet represent each fetcher's T1/T2 bus actions or the object transaction
ordering as explicit per-dot states. Do not port that predicate into the
coarse model in isolation.

### Catch-up predicate discriminator (2026-10-25)

A temporary PPU public-interface sweep varied five transparent-object X
positions (1, 3, 8, 11, 16) and the SCY-write dot from 80 through 145. A
diagnostic alternative continued BG catch-up until high-plane data had been
read and the BG FIFO was non-empty. It changed visible pixels in 11 of the 330
cases. The existing `testObjectFetchStopsBackgroundCatchUpWhenBgFifoHasPixels`
case (X=11, line 1 dot 100) is one of those discriminators: the two schedules
produce different colors in the next BG tile. This confirms the regression is
observable through the public PPU framebuffer.

The alternative changes only the stopping predicate. It does not implement
SameBoy's full per-dot object schedule or its additional fetcher advances, and
it retains LotusGB's precomputed object-event duration. The selected Mealybug
`m3_scx_high_5_bits` ROM still differs in 84 pixels under both models, so it
does not decide this catch-up question. A temporary hand-coded ROM attempt
also failed to land the SCY write at a confirmed PPU dot; its matching outputs
across cores are not valid evidence. No hardware behavior was inferred and no
production timing was changed.

Keep the existing regression as a guard for the current Pan Docs
interpretation, not as proof of DMG hardware behavior. Before changing this
predicate or OBJ event durations, get an independently measured DMG result or
a ROM with a hardware-backed expected image. The temporary diagnostic scan
was discarded; do not add more tests that simply encode the same unverified
assumption.
