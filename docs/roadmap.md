# Development roadmap

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
visual and performance acceptance was not separately recorded. GBC remains
deferred.

## Current route (2026-10-31)

Work in the order below. These are acceptance milestones, not calendar
commitments. M1–M3 are complete. M4 is closed for now without a confirmed fix
or core defect. The current personal-use DMG/libretro baseline is complete;
M5 is complete for the bounded macOS and Steam Deck frontend scope.
M6 is also complete; no next milestone has been selected.

| Order | Milestone | Deliverable and exit condition |
| --- | --- | --- |
| M1 — complete (2026-10-28) | Meaningful game progress | The pinned Adjustris run visibly fills and clears a bottom row, then moves the next piece right. The local checker rejects an incomplete row and a post-clear piece that does not respond. See the [profile and report](homebrew-check.md#adjustris-row-clear-and-post-clear-control-check). |
| M2 — complete (2026-10-28) | Repeatable save and resume | The default Big2Small profile completes Peanut Pathway, compares its saved 02% progress with an empty 00% slot, restores SRAM in fresh core processes, and verifies further directional input. This is runner/core evidence; the separate RetroArch disk-persistence evidence remains distinct. |
| M3 — complete (2026-10-29) | Steam Deck playable checkpoint | Five-minute session on the hash-identified cross-compiled core; normal play, audio, and save progress after reopening the game. An intermittent tearing report remains documented; see the [session record](steam-deck-check.md). |
| M4 — closed for now (2026-10-29) | Intermittent Steam Deck tearing | The report did not recur in the latest Deck check, and Mac testing shows no tearing. No cause or core defect was confirmed. Reopen only if it becomes reproducible; see the [diagnostic record](dmg-validation.md#yellow-intro-lightning-investigation-2026-10-29). |
| Current status — complete (2026-10-29) | Personal-use DMG/libretro playable baseline | The tested Mac and Steam Deck use cases are sufficient for the current goal. This closes the current work round, not the full-emulator compatibility gap list. |
| M5 — complete (2026-10-31) | DMG save states | Core/libretro automated checks pass. RetroArch 1.22.2 saved GB Pong, restored its scene after advancing, and accepted further directional input. The same slot restored after a full frontend relaunch. Steam Deck same-session and full-relaunch restore/control also pass by user report; installed hash and versions were not supplied. See [Mac evidence](save-states.md#macos-retroarch-acceptance-2026-10-30) and [Deck evidence](steam-deck-check.md#m5-save-state-checkpoint--user-reported-pass-2026-10-31). |
| M6 — complete (2026-10-31) | RetroArch rewind | Existing serialization supports the bounded GB Pong Mac session: score/paddle regression, release and continued control, repeated rewind, and selected near-60 FPS observations. Deck R rewind and control after release pass in Ping Pong by user report; exact ROM/core hash, versions and measured performance were not supplied. Update the installed `.info` as well as the library; old metadata can disable rewind. See [evidence and handoff](rewind.md). |
| Future scope — deferred | Optional expansion | More homebrew titles, a standalone GUI, Windows delivery, CGB, and other features need a concrete user or distribution target before becoming milestones. |

### Current scope decision — personal-use DMG/libretro baseline complete

On 2026-10-29, the user agreed that the current playable baseline is enough to
close this work round. The intended use is playing selected DMG games through
RetroArch on macOS and Steam Deck. Evidence includes targeted homebrew gameplay
and save/resume checks, plus one five-minute Steam Deck session on the
hash-identified Linux core. The intermittent Deck tearing report did not recur
in the latest check; Mac testing also showed no tearing. These results do not
claim broad commercial-game compatibility or exact hardware accuracy.

Keep the project scoped as a DMG libretro core for now. RetroArch supplies the
user interface, so a custom GUI is not part of the current scope. Windows
delivery is deferred until there is a concrete Windows user or release target;
the Linux Steam Deck build does not count as native Windows validation. Adding
more homebrew games remains an optional expansion, not a prerequisite for
closing this baseline. Known hardware and compatibility gaps remain in the
[DMG validation matrix](dmg-validation.md).

The user subsequently selected instant save/load as the next product feature.
M5 extends the completed playable baseline with session convenience. It does
not reopen the unconfirmed tearing report or claim broader game compatibility.

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

The 2026-10-26 user report confirms normal F-1 Race and Pokémon Yellow play,
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
Deck report, the user identified Pokémon Yellow's opening Pikachu lightning
animation as a scene to watch. The user later reported no tearing in the latest
Deck check and no tearing during Mac testing. No production core code or Deck
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
| CGB, NES, GBA and other systems | A separate scope decision follows the DMG checkpoint. | Define one target and acceptance game before adding cross-system abstractions. |

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
