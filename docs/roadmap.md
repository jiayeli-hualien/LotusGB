# Development roadmap

## Current route — GBA single-player playability and fidelity (2026-11-09)

The bounded Emerald checkpoint is passed: the game reached Route 102, accepted
directional play, and resumed its in-game save after closing and reopening the
runner. The GPIO RTC also advanced across reload, meeting the selected
acceptance condition; berry growth and watering are not required.

Licensed audio fixtures cover PSG, Direct Sound, SOUNDBIAS and libretro sample
delivery. An original Mode 0 tile/sprite fixture checks every frame pixel
through the system and libretro callback in both pixel formats. `make test`
passes these with the repository suite. Emerald Mono/Stereo routing was
observed, but sound quality has not been compared with hardware; no
target-specific rendering or PCM defect is currently recorded. Continue
implementation when a concrete Emerald or licensed homebrew discrepancy is
reproduced, rather than adding speculative timing changes.

Link Cable, RFU and GBA save states remain deferred. See the [GBA status and
evidence](gba-development.md) for implementation limits and test details.

The bounded gameplay and Steam Deck checkpoints are complete. The intermittent
Deck tearing report is closed for now: it did not recur in the latest Deck
check, and the user reports no tearing during Mac testing. No core defect was
confirmed. Reopen the investigation if the issue becomes reproducible.

DMG instant save/load through RetroArch (M5) passed its bounded macOS
frontend checkpoint on 2026-10-30 and the Steam Deck checkpoint by user report
on 2026-10-31. The latest Deck session also showed no tearing.
M6 rewind validation is complete for the bounded Mac and Deck scope: the Mac
GB Pong session passed, and the user confirmed R rewind and normal control
after release in Ping Pong on Deck. Shader startup works by user report;
visual and performance acceptance was not separately recorded. M7's bounded
automated input/presentation checkpoint is complete. M8 passed its first native CGB-E playability checkpoint: color Acid2,
Tobu Tobu Girl Deluxe controls/state replay and a macOS RetroArch session.
M10's bounded CGB-E Pokémon Crystal single-player target also passed user
acceptance after the current core update: gameplay, saving and save persistence
after reopening Steam RetroArch. Broader CGB hardware accuracy and game
compatibility remain separate work.

Rewind-audio follow-up (2026-11-02): snapshot serialization now reuses the
immutable ROM identity rather than hashing the full ROM on every frame. The
user reports clean audio during forward play with rewind enabled in Mac's
Steam RetroArch after its separate core directory was updated, and reports
normal audio on Steam Deck as well. The user also confirmed normal audio while
holding Rewind and after releasing it. The Deck core hash and session duration
were not recorded. See the [rewind evidence](rewind.md).

## Earlier DMG/CGB route and milestones (2026-11-03)

This section records the prior DMG/CGB route and acceptance results. These
are acceptance milestones, not calendar commitments.
M1–M3 are complete. M4 is closed for now without a confirmed fix
or core defect. The current personal-use DMG/libretro baseline is complete;
M5 is complete for the bounded macOS and Steam Deck frontend scope.
M6 is also complete. M7's headless input/audio/state checkpoint is complete:
bounded VBlank presentation is now the default, with fixed stepping retained
for diagnostics. Broader manual input-feel and physical-latency checks remain
deferred and do not block further core development. M8 now passes its bounded
native-CGB playability checkpoint;
M9 adds a bounded CGB gameplay and battery-SRAM regression. Broader CGB title
compatibility remains exploratory; public regressions use original fixtures
and licensed homebrew.

Routine Steam Deck revalidation is deferred by user decision. Use macOS Steam
RetroArch as the representative frontend for ongoing acceptance; retain the
existing Deck results and reopen Deck checks if a platform-specific issue or a
release target makes them necessary. Similar RetroArch behavior across
platforms is the working assumption, not a claim that every host difference
has been tested.

| Order | Milestone | Deliverable and exit condition |
| --- | --- | --- |
| M1 — complete (2026-10-28) | Meaningful game progress | The pinned Adjustris run visibly fills and clears a bottom row, then moves the next piece right. The local checker rejects an incomplete row and a post-clear piece that does not respond. See the [profile and report](homebrew-check.md#adjustris-row-clear-and-post-clear-control-check). |
| M2 — complete (2026-10-28) | Repeatable save and resume | The default Big2Small profile completes Peanut Pathway, compares its saved 02% progress with an empty 00% slot, restores SRAM in fresh core processes, and verifies further directional input. This is runner/core evidence; the separate RetroArch disk-persistence evidence remains distinct. |
| M3 — complete (2026-10-29) | Steam Deck playable checkpoint | Five-minute session on the hash-identified cross-compiled core; normal play, audio, and save progress after reopening the game. An intermittent tearing report remains documented; see the [session record](steam-deck-check.md). |
| M4 — closed for now (2026-10-29) | Intermittent Steam Deck tearing | The report did not recur in the latest Deck check, and Mac testing shows no tearing. No cause or core defect was confirmed. Reopen only if it becomes reproducible; see the [diagnostic record](dmg-validation.md#yellow-intro-lightning-investigation-2026-10-29). |
| Current status — complete (2026-10-29) | Personal-use DMG/libretro playable baseline | The tested Mac and Steam Deck use cases are sufficient for the current goal. This closes the current work round, not the full-emulator compatibility gap list. |
| M5 — complete (2026-10-31) | DMG save states | Core/libretro automated checks pass. RetroArch 1.22.2 saved GB Pong, restored its scene after advancing, and accepted further directional input. The same slot restored after a full frontend relaunch. Steam Deck same-session and full-relaunch restore/control also pass by user report; installed hash and versions were not supplied. See [Mac evidence](save-states.md#macos-retroarch-acceptance-2026-10-30) and [Deck evidence](steam-deck-check.md#m5-save-state-checkpoint--user-reported-pass-2026-10-31). |
| M6 — complete (2026-10-31) | RetroArch rewind | Existing serialization supports the bounded GB Pong Mac session: score/paddle regression, release and continued control, repeated rewind, and selected near-60 FPS observations. Deck R rewind and control after release pass in Ping Pong by user report; exact ROM/core hash, versions and measured performance were not supplied. Update the installed `.info` as well as the library; old metadata can disable rewind. See [evidence and handoff](rewind.md). |
| M7 — bounded automated checkpoint complete (2026-11-01) | Input response and presentation pacing | Tobu's tested short/held actions now respond in call 2 under the default bounded VBlank policy, versus call 3 in the fixed diagnostic baseline. The policy distinguishes real completion, healthy host repeat, STOP and failure; hardware clocks remain shared. Dual-mode audio/coherent-video/state/rewind regressions pass. See the [current policy and evidence](input-latency.md#current-presentation-policy). Manual Mac/Deck feel, listening and physical latency are deferred, not claimed as passed. |
| M8 — playable checkpoint passed (2026-11-01) | Native CGB-E through RetroArch | Banked RAM, color PPU, KEY1 speed switching, VRAM DMA and model-tagged snapshots. Exact cgb-acid2 pixels and Deluxe directional/flap/dash response with state replay pass; installed macOS RetroArch loads/restores gameplay. Broader game compatibility and hardware accuracy remain work. See the [canonical evidence and limits](cgb-development.md). |
| M9 — bounded checkpoint complete (2026-11-01) | CGB gameplay and battery SRAM | CI fixtures cover CGB-compatible MBC1 and MBC5, color/input gameplay, battery-backed progress and fresh-content SRAM restore. Optional local `make test-cgb-homebrew` adds Big2Small MBC5 save/resume and Deluxe MBC1 gameplay/state replay. See [CGB evidence](cgb-development.md#reproducible-checks). |
| M10 — bounded user acceptance passed (2026-11-04) | CGB-E Pokémon Crystal single-player playability | Original CGB fixtures cover MBC1/MBC5 gameplay and battery RAM, MBC5 ROM banks $100/$1FF and 128-KiB SRAM gameplay/state/battery replay, plus MBC3 guest RTC latch/read/write, running-clock offline progression/day rollover and fresh-content battery import in both libretro presentation builds. The user reports Crystal single-player playability and accepted the updated macOS Steam RetroArch core after confirming gameplay, in-game saving and save persistence after reopening. ROM region and route/coverage were not recorded; this closes the bounded personal-use target, not all CGB game compatibility, full-story verification or real-title RTC progression. See the [CGB playability record](cgb-development.md#pokemon-crystal-post-update-check-2026-11-04). Keep ROMs and derived artifacts private; use licensed homebrew and original fixtures for reproducible public checks. |
| GBA — Pong and battery-save checkpoint passed (automated, 2026-11-08) | Independent LotusGBA libretro core | The callback test passes the MIT ARM and Thumb instruction suites, then runs the MIT-licensed Pong-Homebrew-GBA ROM through its tested gameplay path. Original fixtures verify SRAM, 64-KiB Flash, banked 128-KiB Flash, and DMA-driven 512-byte/8-KiB EEPROM read/write and retention; libretro save-memory exposure is checked for each family. This is implementation and synthetic-test evidence, not manual RetroArch acceptance or commercial-game save validation. At this checkpoint, RTC was not yet implemented; accurate timing, game-level audio evaluation and other video modes remain follow-up work. See [GBA status and evidence](gba-development.md). |
| GBA — Mode 0 libretro graphics baseline passed (automated, 2026-11-09) | Original tile-and-sprite homebrew fixture | The original 4bpp Mode 0 test cartridge draws a red tile background and a 16×16 green sprite through BIOS `CpuSet`. The libretro callback now compares all 240×160 pixels against the reference in both XRGB8888 and 0RGB1555; the existing direct-system test checks the same frame. This covers the frontend integration path, not additional hardware behavior. See [GBA graphics test coverage](gba-development.md). |
| GBA — basic audio path passed (automated, 2026-11-09) | PSG, Direct Sound FIFO and audio output | Synthetic checks cover PSG panning, Direct Sound A/B separation and 50%/100% volume, timer-driven FIFO playback, SOUNDBIAS clipping/cadence and libretro sample delivery. A private Route 102 probe confirms the game's Mono option produces matching channels and Stereo produces separated left/right PCM. This verifies game-driven routing, not sound quality; comparison with hardware remains open. See [GBA status and evidence](gba-development.md). |
| GBA — Sound 3 wave banks and output modes (automated, 2026-11-09) | GBA wave RAM and channel 3 playback | The licensed audio test path verifies CPU access to the inactive 16-byte bank, 32-step versus 64-step playback across both banks, the force-75%-volume bit, and ignored high-byte register bits. This fills the documented bank/dimension gap but does not assess analog output or game-level sound fidelity. See [GBA implementation status](gba-development.md). |
| GBA — WAITCNT first timing stage passed (automated, 2026-11-08) | Register masks and basic CPU-side Game Pak waits | Original CPU loops verify WAITCNT reset/readback/reserved-bit handling and show that ROM first/sequential waits and Game Pak RAM waits change work completed in one emulated frame in the expected direction. The prefetch buffer and complete CPU/DMA timing remain out of scope for this stage. See [GBA status and evidence](gba-development.md). |
| GBA — Emerald early-game save/reload and RTC checkpoint passed (2026-11-09) | Private Pokémon Emerald single-player probe | Earlier private play reached Route 102, saved in-game and resumed there after reload. A fresh-source headless libretro check later loaded a saved game with 0:30 playtime, restored the outdoor map, and moved the player right; the Flash image remained byte-identical. Its RTC sidecar had been offline about 8 hours 45 minutes; reload advanced the persisted time base while keeping the effective clock continuous. The user accepts time advancement after reload as sufficient RTC verification. This is an early-game checkpoint, not full-story or hardware-accuracy coverage, and the headless check is not RetroArch frontend acceptance. ROMs, saves and captures remain private. See [GBA evidence](gba-development.md). |
| GBA — SOUNDBIAS output stage and licensed audio baseline passed (automated, 2026-11-09) | MIT GBA Audio Check ROMs | One ROM produces a deterministic PSG tone measured in the 420–460 Hz range and routed equally to both channels; a second alternates signed samples through timer/DMA-driven Direct Sound A/B and measures a 1950–2150 Hz output, routed right and left. Tests verify separate Direct Sound sides, the 50%/100% volume ratio, 10-bit bias/clipping, 9–6-bit PWM amplitude quantization, 32.768–262.144 kHz SOUNDBIAS cadences, high-rate attenuation before 48 kHz conversion, and unused register-bit masks. The current four-pole 18 kHz filter passes the licensed alias-rejection check. A private Emerald capture confirms non-silent, unclipped PCM after this update, but no hardware sound comparison has been made. See [ROM source and terms](../tests/roms/gba-audio-check/README.md) and [GBA implementation status](gba-development.md). |
| Future scope — deferred | Optional expansion | More homebrew titles, a standalone GUI, Windows delivery and systems beyond GBA need a concrete user or distribution target before becoming milestones. |

### Paused diagnostic route — GB/CGB specification coverage

This route is paused while the current target is GBA. The remaining GB/CGB
technical notes below are a diagnostic reference if a concrete regression
requires reopening that work; they are not the current next steps.

CGB VBlank/STAT timing now passes Mooneye's CGB-targeted
`misc/ppu/vblank_stat_intr-C`: line-144 Mode 2 STAT asserts one M-cycle before
VBlank, while the DMG control remains at VBlank entry. Focused PPU checks cover
native CGB and CGB compatibility. BIOS-produced register/divider cases are
now isolated in a separate optional Mooneye group that requires an external
CGB boot ROM; see the [CGB evidence](cgb-development.md#cgb-vblankstat-interrupt-edge-2026-11-03).

The latest bounded correction separates native CGB OAM DMA cartridge and WRAM
bus ownership. Focused bus tests reproduce the previous failure; an original
CPU probe covers ROM execution/SRAM writes during WRAM DMA and in-flight
snapshot replay. See the [CGB evidence](cgb-development.md#cgb-oam-dma-bus-separation-2026-11-02).
CGB same-bus fetch collisions now also have a guest CPU probe: a WRAM DMA byte
executes as code while the free cartridge bus accepts an SRAM write; the DMG
single-bus control blocks the write. A complementary ROM-source probe executes
DMA bytes in place of a HALT ROM fetch and checks SRAM write blocking until bus
release. Another original CPU probe runs `PUSH BC`/`POP DE` on a WRAM stack
during ROM-source DMA and checks the recovered word after the cartridge bus is
released. Other conflict phases and revisions remain open (see
[evidence](cgb-development.md#cgb-oam-dma-fetch-conflicts-2026-11-03)).
CGB OAM DMA source reads also have decoy-bank checks for VBK-selected VRAM and
SVBK-selected WRAM (see
[evidence](cgb-development.md#cgb-oam-dma-banked-sources-2026-11-03)).
ROM-window source data also has a guest-level check: an MBC1 program executes
the transfer routine from WRAM, then copies and compares all 160 OAM bytes in
SRAM.
CGB GDMA and LCD-off HBlank DMA now also check that WRAM sources use the
SVBK-selected bank rather than bank-1 decoy data (see
[evidence](cgb-development.md#cgb-vram-dma-banked-sources-2026-11-03)).
CGB GDMA and LCD-off HBlank blocks now also have direct `SystemClock` and
guest-CPU integration checks proving their 32-base-T-cycle rate stays fixed at
normal and double CPU speed (see
[evidence](cgb-development.md#cgb-vram-dma-rate-at-double-speed-2026-11-03)).
CGB GDMA now consumes its transfer during Mode 3 while dropping destination
writes blocked by the PPU; a focused test also confirms writes resume in
HBlank. A destination-overflow regression confirms that transfer stops at the
end of VRAM instead of wrapping to `$8000`; the overflowed `FF55` readback is
left unspecified. The [CGB VRAM DMA guide](cgb-development.md#gdma-writes-while-the-ppu-owns-vram-2026-11-03)
records the documented behavior and remaining source/OAM-DMA conflict limits.
CGB OAM DMA now has both a guest-visible normal/double-speed scanline check and
a `SystemClock` boundary regression: it checks the first byte at M=2, the last
byte, and release exactly 160 CPU M-cycles later (640/320 base-clock dots).
The line-2 object is hidden while normal-speed DMA remains active and visible
after double-speed DMA finishes before that scan ([evidence](cgb-development.md#cgb-oam-dma-rate-at-double-speed-2026-11-03)).
HBlank DMA also now pauses during CPU HALT and resumes after VBlank wake;
an original guest program verifies retained blocks, copied data and exact
snapshot replay (see [evidence](cgb-development.md#hblank-dma-and-halt-2026-11-02)).
CGB HBlank DMA now also follows the documented LCD-off behavior: one block
runs when disabled, then remaining blocks wait for visible HBlanks after LCD
restart. Component and guest CPU tests cover both startup-off and mid-transfer
shutdown, FF55 progress, source data and exact snapshot replay (see
[evidence](cgb-development.md#hblank-dma-and-lcd-off-transitions-2026-11-03)).
An additional PPU/DMA regression checks that a block armed after line 143's
HBlank remains pending through all VBlank lines and resumes at line 0 of the
next frame, matching Pan Docs' visible-line HBlank schedule (see
[evidence](cgb-development.md#hblank-dma-across-vblank-2026-11-04)).
Native CGB now has model-specific active-wave-RAM access, CH3 retrigger and
powered-off length-register behavior, with component/guest CPU tests and exact
snapshot replay (see [APU evidence](cgb-development.md#cgb-apu-register-behavior-2026-11-02)).
CGB PCM12/PCM34 now expose live digital outputs through the shared APU and
CPU bus, with component/model/CPU checks and snapshot replay (see
[PCM evidence](cgb-development.md#cgb-pcm-observation-registers-2026-11-02)).
Pulse first-duty suppression and retrigger countdown low-bit retention now
have PCM regressions. Snapshot v3 records the new flags and loads legacy DMG
v1/CGB v2 states (see [pulse evidence](cgb-development.md#pulse-first-duty-and-retrigger-phase-2026-11-02)).
Documented NRx4 extra length-enable/trigger clocks now have all-channel,
eight-phase component tests plus CPU/DIV/NR52 checks and snapshot replay (see
[length evidence](cgb-development.md#length-enable-and-trigger-clocks-2026-11-02)).
Envelope trigger phase-7 reload now has all-phase/period/direction digital
regressions and CPU/DIV observation with pending-tick snapshot replay (see
[envelope evidence](cgb-development.md#envelope-trigger-reload-phase-2026-11-02)).
CH1 sweep now retains the pending pulse countdown and acts on trigger-time
overflow, with PCM/control and NR52 regressions (see
[sweep evidence](cgb-development.md#ch1-sweep-countdown-and-overflow-2026-11-02)).
The portable serial external-clock interface now has original two-port and
two-system CPU tests, all four CGB clock rates and paired snapshot replay (see
[serial evidence](serial-link.md)). The remaining-specification audit selected
optional native-CGB BIOS loading next: libretro now has an original boot/render/
handoff/reset/state checkpoint and explicit size/identity diagnostics (see
[BIOS evidence](cgb-development.md#optional-cgb-bios-through-libretro-2026-11-02)).
The next audited gap is the DMG STAT-write interrupt quirk, absent on CGB:
it now has a four-clock transient, model/mode/LYC coverage and pending-write
snapshot replay (see [STAT evidence](dmg-validation.md#stat-write-interrupt-quirk-2026-11-02)).
Snapshot v4 carries that transient while loading legacy v1/v2/v3.
The audit also selected standard MBC1M multicart wiring: header-based detection,
full-register bank tests and DMG/CGB CPU/state/battery checks now cover this
previously unsupported variant (see [MBC1M evidence](cgb-development.md#mbc1m-multicart-wiring-2026-11-02)).
CGB high-pass output now uses its documented model-specific charge factor,
with constant-input PCM decay and disconnect tests (see
[filter evidence](cgb-development.md#model-specific-high-pass-decay-2026-11-02)).
DMG Mode-2 OAM corruption now has documented CPU IDU/access events, golden
read/write/combined patterns, the Read+IDU row cutoffs, model controls,
CPU/snapshot replay and a row-by-row M-cycle boundary sweep (see
[OAM evidence](oam-corruption.md)). Sub-M-cycle bus phase, hardware revisions
and DMA-interaction behavior remain outside that bounded model.
CGB speed-switch pauses now latch PPU video-memory permissions by entry mode,
with all-mode background/object/black output checks, CPU switches in both
directions and exact paused snapshot replay. CGB v5 records that latch and loads
legacy CGB v2/v3/v4; DMG remains v4 (see
[speed-switch evidence](cgb-development.md#speed-switch-ppu-locking-2026-11-02)).
The audio audit also corrected all-channel digital-to-DAC conversion, including
inactive-generator bias and CH3 polarity, with all-level/model/PCM/gain tests
(see [DAC evidence](cgb-development.md#digital-generation-and-dac-conversion-2026-11-02)).
Automatic envelopes now distinguish the final valid volume from an attempted
out-of-range step, retain period-zero divider phase and restart on trigger.
All-period/direction tests and a dual-model CPU/DIV snapshot probe cover this
behavior. Snapshot v6 stores separate envelope-update flags and loads previous
model versions (see [envelope stop evidence](cgb-development.md#envelope-divider-and-automatic-stop-2026-11-02)).
The shared period-zero `x8` to `$08` write case now steps active CH1/CH2/CH4
volume, with CGB PCM and DMG mixed-PCM regressions; other revision-specific
zombie-write behavior remains open (see [APU evidence](cgb-development.md#common-period-zero-nrx2-write-step-2026-11-02)).
Pulse duty clocks now continue after output is disabled by length, DAC or
sweep. Golden phase tests and an original CPU/PCM continuous-reference probe
cover retrigger phase and muted-interval snapshot replay without a new format
(see [silent pulse evidence](cgb-development.md#pulse-duty-clocks-while-output-is-disabled-2026-11-02)).
CGB boot-selected DMG compatibility now has an automated checkpoint covering
KEY0/OPRI locking, native-register gating, palette-remapped rendering and exact
pre/post-lock state replay. Libretro exposes `auto`/`dmg`/`cgb` hardware selection;
both pixel formats, reset and rewind have original-fixture checks. At that
checkpoint, CGB v8 recorded bootstrap selections, IR receive settling and the
latched input; it loaded CGB v2–v7, while DMG remained v6. See the
[compatibility contract and remaining limits](cgb-compatibility.md).
Real BIOS revisions and detailed PPU arbitration remain unverified; continue the
important-specification audit. Detailed PPU/DMA conflicts, speed-switch revision
behavior, frontend link transport and analog IR sensor response remain gaps;
connected digital IR has an automated host/core contract ([IR evidence](cgb-infrared.md)).
Native CGB LCDC.0 priority behavior now has a paired renderer regression against
DMG compatibility: CGB keeps BG/Window pixels but lets objects win while the bit
is clear; compatibility mode blanks BG/Window. The test also checks the
"behind BG" object flag in both modes (see
[CGB PPU evidence](cgb-development.md#native-cgb-lcdc0-bgobj-priority-2026-11-04)).
DMG STOP now has an original CPU regression for an already-low selected
Joypad line: only a fresh falling edge after release wakes execution. This
records the documented edge contract, not physical-DMG timing validation.
MBC7 is now supported through the shared mapper: tests cover its serial EEPROM,
accelerometer latch, battery block, libretro sensor fallback and DMG/CGB snapshots
during partial commands and programming. DO busy/ready polling, operation delays
and sequential reads follow the 93LC56 interface; sensor calibration/settling
and per-device program timing remain approximate. See the
[MBC7 evidence](cgb-development.md#mbc7-eeprom-and-accelerometer-path-2026-11-02).
MBC5/MBC7 rumble now preserves register bit 3 separately from RAM gating/bank
selection and drives the optional libretro strong motor. Synthetic-core tests
cover on/off requests, state restore, unload cleanup and the frontend-unavailable
fallback; this does not claim manual motor or game acceptance. See the
[CGB mapper evidence](cgb-development.md#implemented-contracts-and-evidence).
Native CGB now resets the Window Y condition when LCDC.5 is cleared; DMG and
CGB DMG-compatibility mode retain it. An original tile fixture checks hiding,
future-WY re-arming and the model split against the
[Pan Docs behavior](cgb-development.md#native-cgb-window-y-condition-reset-2026-11-02).
OAM DMA activity now reaches the PPU scan boundary: active Mode-2 DMA hides
that line's objects on DMG and CGB even when the transferred bytes equal OAM's
existing contents. Mode-3 object fetches now sample the aligned DMA destination
word for tile/attributes, with an original pixel fixture covering DMG and CGB;
a CPU-driven DMG snapshot fixture replays an active transfer with a fetch pending.
First/last transfer timing and revision-specific scan results remain open
([evidence](cgb-development.md#oam-dma-during-mode-3-object-fetch-2026-11-02)).
Mode 2 now accumulates OAM candidates over its 80-dot window instead of reading
the whole table at Mode 3 entry. The focused PPU regression checks that a DMA
write after an entry's scan point cannot retroactively change its Y eligibility;
the CPU-driven snapshot regression resumes mid-scan and reproduces exact video
and state; DMG Acid2 remains an exact reference-frame regression. The scan
cadence is based on the PPU timing reference, with physical-DMG validation still open.
The scan state fits the already serialized OBJ-event fields; legacy snapshots
finish their current line with the previous bulk-selection behavior. See the
[PPU validation record](dmg-validation.md#target-model-and-ppu-acceptance) and
[scan investigation](ppu-scrolling.md).
The eleven invalid base opcodes now hard-lock instruction fetch and interrupt
service while the system clock continues; DMG v11/CGB v13 snapshots retain the
lock, and reset clears it. CPU, system, frontend and snapshot regressions cover
this contract from the [documented CPU behavior](https://eldred.fr/pandocs/CPU_Instruction_Set.html).
The default Mooneye PPU profile now includes three formerly extended cases:
sprite Mode 0 timing, SCX/HBlank LY timing and STAT IRQ blocking. All 14
selected PPU ROMs pass, and the curated `all` profile is now 91/91.
The DMG interrupt audit also added Mooneye `interrupts/ie_push`: the CPU now
resolves IE changes from the upper-byte stack push before acknowledging IF,
supports cancellation/re-vectoring, and keeps lower-byte changes too late.
Focused CPU regressions cover all three cases. The curated local Mooneye suite
now includes 17 CPU/flag/timing ROMs, DIV timing and serial boot-clock alignment;
the combined 91-ROM set passes. See the
[interrupt contract](interrupts.md#request-wake-and-service).
An original CPU regression now covers the already-pending `EI; HALT` case:
the HALT bug makes interrupt return target HALT itself, and `RETI` re-executes
it after acknowledging the request. The full local Mooneye interrupt group
still passes 11/11, including its distinct later-arriving-interrupt HALT case.
Current gameplay checkpoints do not mean all important GB/CGB specifications
are complete; continue the GB/CGB spec work as a separate automated route.
GBA now has a separate libretro core, passes the licensed ARM and Thumb
instruction suites, runs the tested Pong gameplay path through the libretro
callbacks, and exposes marker-detected SRAM, Flash and DMA EEPROM battery
memory. Synthetic tests cover the save protocols and persistence, but
commercial-game save behavior and manual RetroArch persistence have not been
verified. The core now has automated baselines for PSG audio, Direct Sound FIFO
playback, timer-driven sound DMA, libretro audio callbacks, and the first
WAITCNT-controlled Game Pak wait cycles. The current worktree also implements
the `SIIRTC_V001` GPIO RTC; a private Emerald reload check confirmed that time
advances. Prefetch-buffer behavior, complete CPU and DMA timing, game-level
listening and other video modes remain follow-up work. The MIT-licensed GBA
Audio Check ROMs provide repeatable PSG/SOUNDBIAS and timer/DMA-driven Direct
Sound baselines, including separate left/right routing. Sound 3 now models
both wave RAM banks, 32/64-step playback, inactive-bank access and forced 75%
output. The mixed PSG/FIFO output is sampled at the SOUNDBIAS-selected cadence
before resampling to libretro's fixed 48 kHz output. Rates above 48 kHz pass
through an 18 kHz fourth-order low-pass before resampling. SOUNDBIAS changes
output cadence and quantizes PWM amplitude from 9 to 6 bits; it does not change
PSG oscillator clocks. Other systems remain deferred.

GBA priorities are: first, make recommended single-player games playable
without emulator-introduced bugs (Emerald has now passed an early-game
Route 102 movement and in-game save/reload checkpoint); second, improve
graphics and sound fidelity against hardware behavior; third, defer Link Cable
and RFU. Refine timing when it blocks gameplay or explains a visual/audio
mismatch. For a suspected bug, check documentation, targeted tests or real
hardware before deciding whether it is an emulator defect or behavior to
reproduce.

### Current scope decision — personal-use DMG/libretro baseline complete

On 2026-10-29, the user agreed that the current playable baseline is enough to
close this work round. The intended use is playing selected DMG games through
RetroArch on macOS and Steam Deck. Evidence includes targeted homebrew gameplay
and save/resume checks, plus one five-minute Steam Deck session on the
hash-identified Linux core. The intermittent Deck tearing report did not recur
in the latest check; Mac testing also showed no tearing. These results do not
claim broad commercial-game compatibility or exact hardware accuracy.

The playable frontend now supports DMG and native CGB through one libretro
core. RetroArch supplies the
user interface, so a custom GUI is not part of the current scope. Windows
delivery is deferred until there is a concrete Windows user or release target;
the Linux Steam Deck build does not count as native Windows validation. Adding
more homebrew games remains an optional expansion, not a prerequisite for
closing this baseline. Known hardware and compatibility gaps remain in the
[DMG validation matrix](dmg-validation.md).

The user subsequently selected instant save/load as the next product feature.
M5 extends the completed playable baseline with session convenience. It does
not reopen the unconfirmed tearing report or claim broader game compatibility.

### M7: bounded automated input/presentation checkpoint — complete

The user selected an input-delay assessment on 2026-10-31, including whether
response is adequate for fighting-game feel, and raised CGB as the next
expansion. The selected DMG personal-use baseline is sufficient to continue
development; it does not mean all GB specifications are implemented.

On 2026-11-01, the user selected Tobu Tobu Girl as the first action-game target.
The local checkpoint covers Left/Right, B flap, Right+A dash and Right+B under
short and held inputs. All twelve states respond in call 3 including the
press call. A separate same-BIOS DMG comparison of Right, Right+A and B
responds in LotusGB call 3 versus SameBoy call 2. Event tracing now identifies
the callback/VBlank phase as this case's cause: JOYP and sprite updates occur
in the same game phases, but the affected complete frame reaches different
callback indices. A phase-only control keeps the same emulated update ticks
and completed-frame buffer yet responds in call 2. The user requested a
VBlank test mode; `stepUntilVBlank()` and `make test-tobu-input-vblank` now
provide it, with twelve-state call-2 results. Following the dual-mode headless
gate, the bounded presentation policy was promoted to the default; fixed
stepping remains diagnostic. This does not establish hardware accuracy or
pass low-latency frontend acceptance. See the
[cause, control and current policy](input-latency.md#current-presentation-policy).

The fixed diagnostic baseline preserves one-poll Up pulses in both pixel formats.
Across 24 neutral GB Pong states, first paddle response takes two or three
video calls including the input call; the new default responds in call 2 for
all 24. These results do
not measure physical controller-to-screen latency or certify a fighting game.
See the [measurement definitions, results and pending frontend procedure](input-latency.md).
The complete-frame buffer is preserved while the presentation boundary changes.

The user deferred Mac/Deck manual operation on 2026-11-01. The automated gate
now runs both stepping builds with audio-clock totals, LCD restart/timeout,
coherent video, state restore and repeated rewind/replay checks. A normalized
48 kHz comparison confirms SameBoy also has variable callback-time/sample
totals on the restart stress ROM. The implemented policy returns a real frame
or a bounded healthy Repeat, distinguishing both from STOP/failure and never
synthesizing a guest VBlank IRQ. Fixed-policy snapshots resume without format
migration. This closes the bounded automated checkpoint.
Device feel, listening and physical latency remain deferred; they
are not requirements for continuing core development. See the
[headless results and decision](input-latency.md#headless-pacing-and-rewind-assessment-2026-11-01).


### M8: native CGB-E playability checkpoint — passed

The shared CPU/bus/clock now supports native CGB startup, banked WRAM/VRAM,
color palettes/attributes/priority, speed switching, VRAM DMA and CGB version 2
snapshots. The frontend advertises `.gb|gbc` and chooses the model by header.
Both libretro pixel formats pass the exact cgb-acid2 reference and synthetic
color/input/state/rewind checks. The external Deluxe game responds to held and
one-poll directional/flap/dash input at 12 restored gameplay checkpoints.
The installed core also loaded/restored Deluxe gameplay in macOS RetroArch.

This closes the bounded playable checkpoint, not all CGB hardware work.
Compatibility mode on CGB hardware, revision-specific timing, DMA/bus conflicts,
link/IR and broader game validation remain separate scope. The [CGB guide](cgb-development.md)
owns the feature matrix, reproducible commands, frontend evidence and limits.

### M9: CGB gameplay and battery-SRAM checkpoint — passed

`make test` now runs original CGB-compatible MBC1 and MBC5 game fixtures through
the libretro interface. They check color output, directional response,
progress writes to battery SRAM, and continued progress after unloading,
reloading and importing SRAM into a fresh content instance. The optional local
`make test-cgb-homebrew` profile adds puzzle save/resume in Big2Small and
gameplay input/state replay in Deluxe; both ROMs remain external and outside
CI. This is a bounded integration checkpoint, not broad CGB compatibility
evidence. See [CGB development](cgb-development.md) and the
[testing workflow](testing.md).

### M6: RetroArch rewind — complete

Use the existing M5 serialization through RetroArch's built-in rewind.
The 2026-10-31 Mac GB Pong check passed visible regression and continued
control; no core code or binary changed. An installed old `.info` initially
blocked rewind, so keep its update distinct from library installation. See
the [session evidence, earlier failed attempts and Deck procedure](rewind.md).
The user confirmed Deck rewind with R and normal control after release in
Ping Pong on 2026-10-31, completing the bounded M6 checkpoint. Installed
core/ROM hashes, versions and measured performance were not supplied. Audio
quality, long-session stability, RTC behavior, run-ahead and replay remain
separate targets.

### M5: DMG instant save/load — complete

Implement versioned value-only snapshots at M-cycle boundaries, expose them via
libretro, and verify continuation after a fresh content load. Invalid states
must leave the running game intact; published SRAM/RTC pointers must remain
stable. The format identifies the ROM, BIOS configuration and hardware model.
The [save-state guide](save-states.md) defines ownership, RTC wall-clock policy,
format/versioning and focused automated acceptance checks.

Core/libretro implementation and automated validation are complete. `make
build`, `make test`, and the pinned GBIT comparison passed. On 2026-10-30,
RetroArch 1.22.2 on arm64 macOS passed saving during GB Pong gameplay,
advancing, restoring the scene, and further directional control. A full
frontend relaunch loaded the same on-disk slot and restored the scene again;
the paddle still responded. See the [hash-identified session record](save-states.md#macos-retroarch-acceptance-2026-10-30).
On 2026-10-31, the user reported that both same-session restore/control and
full-RetroArch-relaunch restore/control worked on Steam Deck, with no current
tearing. The [Deck record](steam-deck-check.md#m5-save-state-checkpoint--user-reported-pass-2026-10-31)
notes the required GUI core reinstall and missing installed hash/version
metadata. This closes the bounded M5 acceptance scope; the earlier M3
battery-save evidence remains separate. GBC implementation, rewind/replay and
shader configuration are not part of M5.

### M1: Adjustris row clear and continued control — complete

The pinned CC0 Adjustris profile now selects set 5, whose default pieces are
single blocks, fills the bottom row, and captures the transition through the
clear animation. The next spawned piece moves right after the row disappears.
The local checker inspects the board cells frame by frame and fails if it sees
no complete row, the row does not clear, or the next piece does not move. A
same-set start-only baseline completed the runner checks and was rejected for
having no full row. Focused ROM-free tests cover successful evidence and failure
cases.

On 2026-10-28, `make test-homebrew HOMEBREW_GAME=adjustris` passed with ROM
SHA-256 `9b7bfcd8cec7d85e257c43cc62a9965852d59c9fe94dd9f643de3f225450b58b`
and core SHA-256
`da6a3334e8f6738213cf83f095098521a3b139061eb82c3158239630e89135e2`.
The captured bottom row was full at frame 1063 and cleared by frame 1109;
the next piece moved from column 5 to column 6 at frame 1112. Two identical runs
completed 3,600 video frames, produced 2,893,139 audio frames and an 8 KiB
SRAM image, and reported no validation error. Both final RGB frames had
SHA-256 `cfb547d3b2eaabdaa73afa6c977d6373a3eb537a8c82b89e1a65bd9cfb419d75`.
These checks verify one row clear and a subsequent directional response for
this ROM/core/input profile;
they do not establish full playability, hardware accuracy, sound quality or
save persistence. See the [profile and generated evidence](homebrew-check.md#adjustris-row-clear-and-post-clear-control-check).

The ROM and source remain outside the repository. This local profile is not
part of CI; committing or distributing the ROM requires separate confirmation
of the ROM binary's terms.

### M2: make save evidence repeatable — complete

The default `make test-homebrew` profile completes Big2Small's first puzzle,
returns to the map, and writes its SRAM to the ignored `build/test-roms`
directory. The checker creates an empty 00% save in a separate process, then
compares that title screen with a new process restoring the completed save.
That screen displays 02%. A further process restores the same SRAM, enters the
game, and moves the elephant right; the captured frame changes by 519 pixels.

On 2026-10-28, the empty and completed SRAM hashes were
`e821e06312e8ba9407262a9850424d3ed44b4e64a72ecea6ca23fb48811f9ac3` and
`656420b4a925e3fa66e5ad8c84136d3da632370a856be37e7ef0f5dc49034456`;
four SRAM bytes changed. The restored 00% and 02% title frames differed in 29
pixels. All five independent runner invocations delivered 3,600 video frames,
2,893,139 audio frames and an 8 KiB SRAM image with no validation error. The
commands, input scripts, hashes and captures are in the
[homebrew guide](homebrew-check.md#big2small-save-and-resume-check).

This closes the runner/core save round trip. It does not replace the previously
confirmed RetroArch disk-persistence result, test RTC behavior, or claim
hardware accuracy. Supplying `HOMEBREW_SAVE` retains its read-only inspection
path and skips the fresh-save regression. The M3 session remains a separate
device/frontend check.

### M3: close a bounded Steam Deck checkpoint — complete

The 2026-10-26 user report confirms normal F-1 Race and another Game Boy title play,
working battery saves and scaling, but it does not identify the exact core hash
or RetroArch version. Minor tearing was reported before the completed-frame
output fix. The cross-compiled candidate's later device result is summarized
below. Mac logo tearing reportedly improved. These observations are not a
current confirmed core blocker; see the
[frame-output evidence](dmg-validation.md#dmg-logo-animation-cross-core-comparison-2026-10-27).

The current workspace has no Steam Deck connection. The standard local
libretro artifact remains arm64 macOS, and a separate x86-64 Linux candidate
has now been cross-compiled with Zig 0.17.0 and inspected as an ELF shared
object with glibc requirements no newer than 2.17. A user report describes five
minutes of normal play, normal-feeling audio, and working save/load, with
occasional screen tearing reported; that report is now closed for the time
being after it did not recur on the Deck and Mac testing showed no tearing. The user has
confirmed the loaded core hash matches the cross-compiled candidate. Device/
frontend versions are not recorded. The user reports that reopening the game
restored saved progress, and the five-minute gameplay, audio, and content
reload checks passed. A full RetroArch process relaunch was not reported. M3 is
complete for this bounded device/build/game checkpoint. The intermittent
tearing report has no confirmed cause or core fix. See the
[Steam Deck procedure and result](steam-deck-check.md). A homebrew game is the
reproducible baseline; commercial-game checks remain private and manual.

**Done means:** the selected session has no reproducible failure that prevents
play or loses saved progress. Record observed visual/audio issues and whether
they need follow-up; an unresolved, non-blocking issue can remain on the
follow-up list. This is a device/build/game checkpoint, not blanket DMG
compatibility. If the device is unavailable, mark this milestone awaiting
manual validation; a headless run cannot close it.

### M4: intermittent Steam Deck screen tearing — closed for now (2026-10-29)

The user prioritized tearing over save states. After the initial occasional
Deck report, the user identified a scene-specific visual effect to watch. The user later
reported no recurrence in the latest Deck check or during Mac testing. No production core code or Deck
binary changed during this investigation, so the non-reproduction is not a code
fix. No cause was confirmed; frontend/device settings remain one possibility,
not a finding.

The new callback-level regression varies LCD startup timing and tests both
pixel formats. All 400 callbacks passed, and a temporary negative control
reintroducing live-frame submission failed at pixel `(0, 10)`. `make test`
passed. See the [scoped evidence](dmg-validation.md#frame-coherence-at-the-libretro-callback-boundary-2026-10-29).

The [scene investigation](dmg-validation.md#yellow-intro-lightning-investigation-2026-10-29)
and [callback regression](dmg-validation.md#frame-coherence-at-the-libretro-callback-boundary-2026-10-29)
remain as evidence. Reopen M4 only if a horizontal tear is reproducible again;
then compare the same scene and core behavior before changing the PPU or video
settings. Keep commercial ROMs and game-derived captures outside the
repository and CI.

Investigate only reproduced issues. For audio, compare the same PCM through a
second playback path before changing the APU. For tearing or input response,
separate core output from frontend pacing. Profile performance only when a
measured slowdown or underrun affects this session.

## Established baseline

These are previously recorded results, not checks rerun for this replan.

| Area | Evidence to retain | Limit |
| --- | --- | --- |
| CPU and shared system | M-cycle CPU, bus, interrupts, timer, joypad, serial, PPU, DMA and APU; focused tests and pinned GBIT integration. [Architecture](architecture.md), [validation matrix](dmg-validation.md). | Passing instruction and component tests does not establish complete game compatibility. |
| Startup | Optional external DMG BIOS and fast post-boot startup; selected boot-register, I/O, divider and serial-clock tests pass. [Startup evidence](dmg-validation.md#startup-profile-and-boot-rom-scope). | Fast startup does not reproduce all logo-derived VRAM/OAM state. |
| Rendering | DMG Acid2, selected sprite-priority image, and the expanded 14-case PPU timing group have passing results. [PPU guide](ppu-scrolling.md). | BG/OBJ arbitration and some Mode 3 behavior remain approximate or unverified. |
| Gameplay | GB Pong's visible paddle movement is covered by `make test`; Big2Small completes one puzzle and verifies restored progress/input; Adjustris clears one row and controls the next piece. Tobu Tobu Girl and Canyon Racer remain bounded gameplay checks. [Homebrew evidence](homebrew-check.md). | These profiles do not establish broad game compatibility, audio quality, or hardware accuracy. |
| Frontend and persistence | Ubuntu RetroArch baseline, Steam Deck user reports, SRAM/RTC interface tests and selected disk-persistence observations. [Validation matrix](dmg-validation.md), [historical records](development-history.md). | M3 passed for one hash-identified Deck build; intermittent tearing did not recur in the latest Deck check, and Mac testing showed none. No cause was confirmed; exact device/frontend versions were not recorded. |

## Work selection and validation

- Keep one milestone as the active work item. A reproducible startup, control,
  progress, audio or save defect can interrupt it; record the observed failure
  and acceptance condition first, then return to the milestone after the fix.
- Use focused public-interface tests for core behavior and a game check for
  integration. Follow the [testing workflow](testing.md) for implementation
  changes; optional ROM suites should match the behavior changed. Documentation
  changes require link and consistency checks rather than new emulator runs.
- Reuse current results when the ROM, core, input, initial state and question
  are unchanged. Repeat to establish reproducibility or validate a changed
  behavior, not simply to accumulate another passing report.
- Reference-emulator output is diagnostic evidence. Exact hardware claims need
  model-matched hardware evidence or a hardware-backed expected result. A
  one-callback synthetic input difference or a pixel mismatch alone does not
  justify a timing change.
- Core and libretro failures must remain observable through tests or logging;
  missing output must not become success. Keep ROMs, BIOS data and artifacts
  under the [repository ROM policy](../AGENTS.md#rom-test-policy).

## Deferred work and promotion conditions

| Work | Resume when | First bounded deliverable |
| --- | --- | --- |
| Detailed PPU FIFO/arbitration, OAM corruption and exact timing | A game exposes a concrete effect, or model-matched evidence establishes a specific incorrect behavior. | One reproduced boundary and focused regression. Keep disputed `m3_scx_high_5_bits` optional; no standalone timing sweep. |
| Input/frame phase changes | A playable game reproducibly misses input or shows a control defect, or hardware-backed evidence resolves a specific boundary. | Controlled input-to-visible-result case. Keep completed-frame output and current stepping meanwhile. |
| Additional games, mapper variants, APU or serial behavior | A named target covers an identified gap or exposes a defect. | One observable gameplay or hardware contract; do not claim whole-family coverage. Link play remains unsupported. |
| Performance and cleanup | Profiling finds a player-visible bottleneck, or build/test friction blocks active work. | Measure before/after; preserve behavior. GBIT adapter relocation, endian cleanup, Makefile parallelism and broad source reorganization have no independent deadline. |
| Run-ahead and replay | A user selects one after the save-state checkpoint. | Validate determinism, frontend behavior and performance for that feature; serialization support alone does not establish these results. The bounded M6 rewind checkpoint is complete above. |
| Recalbox | That device becomes an active frontend target. | Verify actual controls, video/audio and saves; the ARM64 headless result already exists. See [Recalbox](recalbox.md). |
| Windows | Windows becomes an active delivery target. | Native build/CI, then RetroArch gameplay and persistence. |
| NES and other systems | A separate scope decision after the active GBA target. | Define one target and acceptance game before adding cross-system abstractions. |

macOS is the current development environment; Ubuntu remains the CI platform.
The bounded Steam Deck checkpoint passed for one identified build; the
intermittent tearing report is closed for now with no confirmed core defect.
Platform work must preserve portable core
behavior and keep operating-system-specific access at the edge.

## Evidence and history

Keep current implementation status in the [DMG validation matrix](dmg-validation.md),
commands in the [testing workflow](testing.md), and game profiles/results in the
[homebrew guide](homebrew-check.md). Update this roadmap when the active task,
acceptance condition or scope changes; do not copy every test run into it.

Dated implementation and investigation records from the previous roadmap are
preserved chronologically in [development history](development-history.md).
Old failure counts and proposed next steps there describe their historical
checkpoint, not current work.
