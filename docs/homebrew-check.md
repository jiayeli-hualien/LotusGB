# Local homebrew regressions

These optional local profiles run freely available homebrew through the built
libretro core. Big2Small is the repeatable gameplay and runner/core
save/resume target; the separate RetroArch disk-persistence result remains
manual. Tobu Tobu Girl and Canyon Racer provide
additional MBC1 and raster-effect smoke coverage. Adjustris fills and clears
a row, then verifies that the next piece responds to input. Other automated
profiles check visible startup/gameplay output and callback delivery. These
bounded profiles do not prove full game compatibility or exact hardware output.
ROMs and player saves stay outside the repository.

## Consolidated local run (2026-10-28)

Big2Small, Adjustris, and Tobu Tobu Girl were each rerun for 3,600 frames
through the same LotusGB core. All primary reports had exit code 0, 3,600/3,600
video frames, 2,893,139 audio frames, an 8 KiB SRAM block, and no validation
error. The Big2Small final image shows its newly unlocked next puzzle; the
Adjustris image shows changed pieces; Tobu Tobu Girl reaches a gameplay scene.
The frames were visually reviewed; these checks do not establish broad game
compatibility or exact hardware output.

| Profile | ROM SHA-256 | Core SHA-256 | Final frame SHA-256 |
| --- | --- | --- | --- |
| Big2Small | `59c096333f93c12c8eb25a5fcffa2be15001abcd5d20b9a82c2bc2dae7e625b2` | `da6a3334e8f6738213cf83f095098521a3b139061eb82c3158239630e89135e2` | `07d948e36f679803c2b0394381d1b5d42cede475c860bc78e23b4eb0830a27dd` |
| Adjustris | `9b7bfcd8cec7d85e257c43cc62a9965852d59c9fe94dd9f643de3f225450b58b` | `da6a3334e8f6738213cf83f095098521a3b139061eb82c3158239630e89135e2` | `cfb547d3b2eaabdaa73afa6c977d6373a3eb537a8c82b89e1a65bd9cfb419d75` |
| Tobu Tobu Girl | `5d3871cae77db2287807e8914fd21f76203e73aa3fec20955489d45cb4571d8f` | `da6a3334e8f6738213cf83f095098521a3b139061eb82c3158239630e89135e2` | `0d99c5dcd3c31e459e99919fd5d8754e6eaa274f748e0386621be52c54676bb7` |

Reports, input schedules, and inspected frames remain under
`build/test-roms/{big2small,adjustris,tobutobugirl}/`.

Run from the repository root:

```sh
make test-homebrew
# Optionally restore a copy of an existing SRAM image into the core:
make test-homebrew HOMEBREW_SAVE=/absolute/path/to/big2small.srm
```

Other profile commands are listed in the [testing workflow](testing.md) and
the relevant sections below.

## Big2Small save and resume check

`HOMEBREW_ROM` defaults to `../test-roms/homebrew/big2small.gb`.
Obtain the free ROM from the [author's page](https://mdsteele.itch.io/big2small).
The accepted ROM SHA-256 is
`59c096333f93c12c8eb25a5fcffa2be15001abcd5d20b9a82c2bc2dae7e625b2`
(64 KiB, MBC5 + RAM + battery). A different release needs explicit review of
the input sequence before updating the pinned hash.
ROMs and player saves are not committed. This optional workflow is outside CI.

The default `make test-homebrew` run starts a new File A, completes the first
Big2Small puzzle, Peanut Pathway, and dismisses its completion screen to return
to the map. The map shows Goat Grove as the next puzzle. The puzzle solution is
the Elephant's Right, Up, Right sequence; see the author's
[game page](https://mdsteele.games/big2small/) for controls and the published
[first-puzzle guide](https://gamefaqs.gamespot.com/gameboy/439191-big2small/faqs/81888).

The save check uses independent 3,600-frame runner processes:

1. Complete Peanut Pathway from fresh SRAM and write `output.srm`.
2. Create a separate 00% slot and restore it in a new process to capture the
   baseline title screen.
3. Restore `output.srm` in another new process and compare its title screen at
   frame 599 with the 00% baseline. The restored screen visibly reads 02%.
4. Restore `output.srm` again in a fresh process, enter the game and press
   Right. The captured game image must change by at least 100 RGB pixels.

On 2026-10-28, all five processes delivered 3,600 video frames, 2,893,139 audio
frames and an 8 KiB SRAM image. The empty-slot SRAM hash was
`e821e06312e8ba9407262a9850424d3ed44b4e64a72ecea6ca23fb48811f9ac3`; the
completed-save hash was
`656420b4a925e3fa66e5ad8c84136d3da632370a856be37e7ef0f5dc49034456` (four
bytes changed). The 00% and 02% title frames differed by 29 pixels. After
restoration, Right changed 519 pixels between frames 2199 and 2300. The run
reported no validation error.

`HOMEBREW_SAVE=/absolute/path/to/big2small.srm` still restores a supplied player
save read-only and checks its hash afterward; that mode skips the fresh-save
round trip. Generated SRAM is never written over a player save. This check
proves runner/core restoration, not RetroArch `.srm` disk writing, RTC behavior,
or hardware accuracy. The separate manual RetroArch persistence result remains
recorded in the [validation matrix](dmg-validation.md).

Artifacts under `build/test-roms/big2small/`:

- `input.txt`: input intervals; zero-based start, exclusive end, button name.
- `empty-slot-input.txt`, `neutral-input.txt`,
  `resume-gameplay-input.txt`: the other process input schedules.
- `runner.log`, `empty-slot-runner.log`, `empty-title-runner.log`,
  `resume-title-runner.log`, `resume-gameplay-runner.log`: callback counts and
  runner errors for the separate processes.
- `frame.ppm`, `puzzle-completed.ppm`, `empty-title.ppm`, `resume-title.ppm`,
  `resume-gameplay-before.ppm`, `resume-gameplay-after.ppm`: inspectable RGB
  frames for the completed puzzle, 00% baseline, restored 02% screen, and later
  input response.
- `frame.rgb`, `output.srm`, `empty-slot.srm`, `resume-title.srm`,
  `resume-gameplay.srm`: raw output frames and SRAM images. All test SRAM files
  stay under the ignored build directory.
- `progress-sequence.rgb`, `resume-title-sequence.rgb`,
  `resume-gameplay-sequence.rgb`: captured frames used by the checker.
- `report.json`: process commands, input scripts, hashes, callback summaries,
  pixel-difference metrics, and validation outcome.

The machine check requires a changed SRAM image, a visible title-frame change
from the restored 00% slot, and a visible Right-input response after loading the
completed save. Review the named PPM files to confirm the displayed 02% value,
completion message, and moved character. Callback counts do not prove audible
sound quality or broad playability. The separate RetroArch save/reload result
is still required for actual frontend disk persistence.

## GB Pong in-process gameplay regression

`make test` loads the repository's MIT-licensed, ROM-only GB Pong release
(`9c5e337db0ee1416f2c7f1de02e7e0a78a6064cf1da49209b4bc1a36a7b35415`)
through the libretro API and runs 60 frames with neutral input and 60 with Up
held. Each run must poll input and publish one frame per call. The visible
acceptance condition is that the left player paddle is in the middle region
with neutral input, then moves into the top region while Up is held. This
checks startup, continued execution, and a game-visible control effect; it
does not establish exact paddle timing, full match behavior, audio quality, or
hardware accuracy. The ROM's source and MIT license are recorded under
`tests/roms/gb-pong/`.

## Local cross-core comparison

To replay a ROM through LotusGB and another libretro core, use
`tools/compare_libretro_cores.py`. For example, the repository includes the
MIT-licensed GB Pong homebrew ROM:

```sh
python3 tools/compare_libretro_cores.py \
  --rom tests/roms/gb-pong/gb-pong.gb \
  --core-a build/lotusgb_libretro.so \
  --core-b /absolute/path/to/sameboy_libretro.so \
  --frames 1200 \
  --system-directory /absolute/path/to/retroarch/system \
  --output-dir build/test-roms/core-comparison/gb-pong
```

The script records ROM, core, runner and input hashes, both final frame hashes,
runner logs and a comparison image. `--inputs` supplies an optional controller
script; `--require-identical` makes a final-frame pixel mismatch return failure.
Each core runs in a fresh empty temporary working directory and receives no
firmware files. The runner normalizes libretro's 0RGB1555, XRGB8888, and RGB565
video formats to RGB24 before comparing. This is a local cross-core check, not
a hardware-accuracy assertion or CI gate.

### SameBoy system-directory correction (2026-10-27)

The initial 3,600-frame SameBoy Tobu run produced a white final image. The
headless runner returned no system directory, while standalone RetroArch is
configured to use `~/Documents/RetroArch/system`, which contains
`dmg_boot.bin`. The runner now accepts `--system-directory` and returns it for
`RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY`; the comparison uses the standalone
RetroArch SameBoy core at
`~/Library/Application Support/RetroArch/cores/sameboy_libretro.dylib`
(`1.0.4 c87d256`, SHA-256
`10b15b7acddd18414d066a4a90f7cf0ea0999ac1ffa66deec7d6ef609060977a`). Its log
confirms it loaded the configured DMG boot ROM. The standalone core completed
3,600 video callbacks and the scripted run ended in an active gameplay scene;
LotusGB completed the same run and both produced identical 8 KiB SRAM hashes.
The final images differ in 13,830 of 23,040 pixels, so this establishes a
working headless SameBoy startup/gameplay path, not pixel accuracy or a
hardware comparison. The earlier no-system-directory capture is invalid and
should not be used as evidence against SameBoy. Logs and images are under
`build/test-roms/core-comparison/tobu-tobu-girl-standalone-system-dir/`.

### Static-to-gameplay comparison (2026-10-27)

Before interpreting the Tobu gameplay mismatch, the MIT-licensed `dmg-acid2`
PPU test ROM was run through the same standalone SameBoy core and LotusGB core,
with the same configured DMG boot ROM and no inputs. At 600 frames both
produced the same visible acid2 test image and identical final RGB bytes
(SHA-256 `2ba8286c29ae381838c71a88614302ce05f2b26102d1ed8dc51e25f83fcccc67`).
The 10- and 60-frame captures were both white, so those were rejected as
premature captures; equality alone was not accepted without checking the
image. The 600-frame artifacts are under
`build/test-roms/core-comparison/dmg-acid2-600f/`.

The previously recorded 3,600-frame Tobu run uses the same LotusGB core hash,
SameBoy core hash, runner, and system directory. It reaches active gameplay,
but its final images differ at 13,830 of 23,040 pixels. This narrows the
observed discrepancy to longer/gameplay execution rather than this settled PPU
test image. It does not by itself identify whether the cause is game state,
timing, or rendering, and neither core is treated as a hardware oracle.

To retain consecutive frames from the runner, set
`LOTUSGB_CAPTURE_SEQUENCE` to an output file and set
`LOTUSGB_CAPTURE_START` / `LOTUSGB_CAPTURE_END` to a zero-based, half-open
frame interval. The file contains tightly packed RGB24 frames in order. For
example, setting the interval to `1600` and `3600` captures frames 1600 through
3599 from a 3,600-frame run. This is intended for local diagnostics; generated
sequences remain under ignored `build/` output.

For the 3,600-frame Tobu profile, consecutive frames 1600–3599 were captured
from both cores with the same ROM, input script and boot ROM. The comparison
shows a sharp SameBoy-only white-screen burst at frames 3342–3346, with the
image returning afterward; the number of white SameBoy pixels rises from
9,229 at frame 3341 to 22,549 at frame 3343. Other bursts occur later in the
run. This is consistent with the suspected hit/invulnerability flash, but
does not prove both cores reached the same collision state. The first burst
contact sheet is `build/test-roms/core-comparison/tobu-frames-3339-3346.png`;
raw frame sequences are in the same directory.

### Big2Small model-aligned comparison (2026-10-27)

After moving development machines, the free ROM was redownloaded from the
[author's page](https://mdsteele.itch.io/big2small) into the external
`../test-roms/homebrew/big2small.gb` directory. Its SHA-256 matches the pinned
release hash. `make test-homebrew HOMEBREW_GAME=big2small` completed 3,600
video callbacks and passed the fixed profile's output checks.

The first SameBoy comparison used its default Auto model, which selected CGB
for this CGB-capable cartridge while LotusGB ran its DMG profile. That run's
color difference is not a valid DMG-to-DMG comparison. The runner now answers
libretro `GET_VARIABLE` from environment variables named
`LOTUSGB_CORE_OPTION_<key>`. Setting
`LOTUSGB_CORE_OPTION_sameboy_model='Game Boy'` forced SameBoy to DMG; both logs
then confirmed the same external `dmg_boot.bin`. With the same 3,600-frame
input script, both cores produced identical final RGB output (0 / 23,040
different pixels) and identical SRAM hashes. This confirms the sampled final
state for this profile only; it does not establish frame-by-frame equivalence.
Artifacts are under
`build/test-roms/core-comparison/big2small-sameboy-dmg/`.

### Controlled input-to-display probe (2026-10-27)

To separate input sampling from moving game objects, a local 32 KiB probe ROM
clears the tile map, samples A once per loop, waits for VBlank, and changes a
fixed 8×8 marker. Its generator and ROM are outside the repository under
`../test-roms/homebrew/input-timing-probe/`; the ROM uses the same configured
`dmg_boot.bin` and SameBoy's model is forced to `Game Boy`. Both cores show the
same boot animation before the scripted input.

With a one-callback A pulse at input frame 520, SameBoy's marker turns black
in output frame 521, while LotusGB's turns black in frame 522. Both return to
white on the following output frame. Exactly the two transition frames differ,
by 64 pixels each. Repeating the pulse at frames 519 and 521 moves each core's
response by the same one frame; SameBoy remains one output callback earlier in
all three runs. Neither core misses the one-frame press. This is direct
support for a one-callback difference from input to visible output in this
controlled ROM. It does not show that all games have a fixed latency offset:
the Big2Small sequence has exact frames between transient mismatches, and its
adjacent-frame offset changes direction at different events. Captures are in
`build/test-roms/core-comparison/input-timing-probe/`.

Both cores were then captured continuously for all 3,600 frames under the same
DMG model, BIOS and input script. 3,182 frames were pixel-identical; 418 had
at least one differing pixel. The first difference was frame 334, the last
was frame 3485, and the largest was 13,745 pixels at frame 1201. SRAM and the
final frame still matched exactly. The frame-1198–1217 contact sheet shows a
short palette/brightness change around the A press at frame 1200, then a
return to the matching scene. This is evidence of transient divergence during
the scripted interaction, not a persistent color mapping error or a proven
cause. In a controlled replay, moving only that A press one frame later moved
the largest divergence to frame 1202; moving it one frame earlier moved the
largest divergence to frame 1200. The transient is therefore tied to the
input event, though its mechanism remains unknown. The contact sheet is
`build/test-roms/core-comparison/big2small-frames-1198-1217.png`; complete
sequences and the ±1-frame replays are under `build/test-roms/core-comparison/`.

A frame-neighbor check also found short periods where a mismatching output
frame is an exact match for the other core's adjacent callback: LotusGB frame
334 equals SameBoy frame 335, and LotusGB frame 339 equals SameBoy frame 340.
The 334→335 transition changes 746 pixels in both cores at the same screen
region (x=32–135, y=64–79), but on adjacent callback indices. Later, LotusGB
frames 3468–3485 equal SameBoy's preceding frames; the corresponding 3467→3468
change is confined to x=34–50, y=64–79, consistent with a moving object. These
are whole-frame matches, not just a sprite-sized region, so the observed
transient includes one-callback differences in game-visible
animation/scene progression.
The direction is not consistent across the run, which argues against a single
fixed output-frame offset. In LotusGB, `retro_run` polls input, advances exactly
one DMG frame, then submits the completed PPU buffer. This confirms the
frontend-facing order on our side; it does not establish whether the remaining
phase shifts come from input sampling, internal frame boundaries, or game
execution timing in either core. The sample count is limited to this scripted
ROM run and is not evidence of an overall one-frame latency.

## General ROM runner interface

```sh
build/libretro_rom_runner CORE.so ROM.gb FRAMES OUTPUT.rgb OUTPUT.srm \
  [INPUT.txt [RESTORE.srm]] [--repeat COUNT]
```

Without optional arguments it preserves the neutral-input ROM-test behavior.
For cores that request legacy libretro `GET_VARIABLE` core options, define
`LOTUSGB_CORE_OPTION_<option-key>` in the runner's environment. For example,
SameBoy's `sameboy_model` can be set to `Game Boy` for a DMG comparison.
`--repeat COUNT` (1–1,000) repeats dynamic core loading, initialization,
cartridge loading, frame execution, unloading, deinitialization, and library
closing in one process. Each run must deliver the requested video frames. The
runner writes the final run's frame and SRAM once, then reports `repetitions`.
Any failed run returns a nonzero exit code. This is a local lifecycle stress
check; use a small frame count for each run. For the included GB Pong ROM:

```sh
make -C src/libretro build rom-test-runner build_dir="$PWD/build"
mkdir -p build/test-roms/lifecycle
build/libretro_rom_runner build/lotusgb_libretro.so \
  tests/roms/gb-pong/gb-pong.gb 2 \
  build/test-roms/lifecycle/frame.rgb build/test-roms/lifecycle/output.srm \
  --repeat 32
```

Input lines contain `START END BUTTON`; blank lines and `#` comments are allowed.
Buttons: `A B START SELECT UP DOWN LEFT RIGHT`. Overlapping intervals permit
combinations. Intervals must fit within the requested run. Restored SRAM must
match the cartridge's exposed SRAM size. An output cannot alias the restored
save, including through a symlink or hard link. To restore without input, supply
an empty input file.

## Tobu Tobu Girl gameplay smoke check

For the newer M7 input-response question, use `make test-tobu-input` and the
[action baseline and reference comparison](input-latency.md#tobu-tobu-girl-action-baseline-2026-11-01).
That optional check measures short/held directions, flap and directional dash;
the older smoke profile below remains a startup/execution check.

```sh
make test-homebrew HOMEBREW_GAME=tobutobugirl
```

The default ROM is `../test-roms/homebrew/tobutobugirl/tobu.gb`, obtained from
[the author's page](https://tangramgames.itch.io/tobutobugirl). The pinned
SHA-256 is `5d3871cae77db2287807e8914fd21f76203e73aa3fec20955489d45cb4571d8f`
(256 KiB, MBC1 + RAM + battery, 8 KiB SRAM). Source and assets have separate
licenses; keep the downloaded ROM external. See [the source project](https://github.com/SimonLarsen/tobutobugirl).

This profile starts without player SRAM, presses START during the opening
scenes, confirms a stage with A, then exercises rightward movement, A dash
and B upward boost. The 3,600-frame run writes the same artifact types under
`build/test-roms/tobutobugirl/`. Review `frame.ppm` for actual gameplay; the
exit status checks execution and callbacks, not scene recognition. Timing is
pinned to this ROM release and may need adjustment if restored progress is
supplied. Big2Small remains the manual battery-save acceptance game; this
profile does not require beating a Tobu Tobu Girl level or writing a high score.

On 2026-10-24, the scripted run reached a visible gameplay scene with the
character, enemies and HUD. The neutral-input run remained in the opening
scene. These observations demonstrate a limited startup/input path, not
complete level compatibility, precise rendering or sound quality.

The same profile was rerun on 2026-10-26. It completed 3,600 video callbacks
and 2,893,139 stereo audio frames, produced an 8 KiB SRAM block, and showed a
gameplay scene. The final frame SHA-256 was
`260f1d3926acb5a2db9b2ec757b6a493158dc4f9de3b98b7e939df484e35c8f4`; the core
SHA-256 was `80ec278b8906ac60ce3f865098cec05c50bddef291461aff5cb5a4963fa1b006`
and the input script SHA-256 was
`2c205fb0ed775c6d8138cef8cb5821ee29e864bb402b88a45a61afd7c44a724c`.
This remains a scripted execution check, not a level-completion or save-
persistence acceptance test.

The same external ROM and input profile were rerun on macOS on 2026-10-27
after the MBC1 bank-switch integration regression was added. The run returned
exit code 0 with 3,600/3,600 video callbacks, 2,893,139 stereo audio frames,
and 8 KiB of SRAM. Manual inspection of `frame.ppm` showed the character,
enemies, and HUD in gameplay. The final frame SHA-256 was
`0d99c5dcd3c31e459e99919fd5d8754e6eaa274f748e0386621be52c54676bb7`; the
core SHA-256 was
`22a4f4b4bffb2863b3f4450d1803e436cd238dff55f490571494417af9c5a3f8`. The ROM
and generated outputs remain outside source control. This confirms the
scripted startup/input path and visible gameplay for this release, not level
completion, audio quality, or save-file persistence.

## Adjustris row-clear and post-clear control check

```sh
make test-homebrew HOMEBREW_GAME=adjustris
```

Adjustris is a DMG block-dropping game. Its author dedicates the software to
the public domain under CC0. The source and ROM stay outside the LotusGB
repository at `../test-roms/homebrew/Adjustris-source/`. The source revision is
`d899e2f539c07ac49fd42c055605bf778a2df09d`; the 32 KiB ROM has SHA-256
`9b7bfcd8cec7d85e257c43cc62a9965852d59c9fe94dd9f643de3f225450b58b`, header
type `$13` (MBC3 + RAM + battery), and 8 KiB SRAM. RGBDS v0.3.10
(`0759c98d913e3d4d21207a8886a319c85add2041`) is needed to rebuild this source.

The 3,600-frame input script selects set 5, whose default pieces contain one
block each, then places ten blocks across the bottom row. It uses the existing
libretro runner's frame capture for frames 1000–1139. The checker samples each
board cell and requires all ten bottom cells to fill, that row to clear within
90 frames, and the next piece to move right while remaining on the same row.
A focused ROM-free regression rejects a missing full row, a row that does not
clear, and a next piece that does not respond. The run still checks the
existing callback and output contract. A same-set start-only baseline is also
captured in the same time window and must fail the full-row condition for the
right reason. It does not claim full playability, hardware accuracy,
audio quality or save persistence.

Two consecutive runs on 2026-10-28 passed with the same ROM and core hashes.
Both found a full row at frame 1063, an empty row at frame 1109, and movement
of the next piece from column 5 at frame 1110 to column 6 at frame 1112. Both
reported 3,600/3,600 video frames, 2,893,139 audio frames, 8,192 SRAM bytes,
no validation error, and final RGB SHA-256
`cfb547d3b2eaabdaa73afa6c977d6373a3eb537a8c82b89e1a65bd9cfb419d75`. The
ROM hash, core hash, input schedule, detected frames and other run details are
in `build/test-roms/adjustris/report.json`.

Review the captured views and raw sequence under `build/test-roms/adjustris/`:
`progress-full-row.ppm`, `progress-row-cleared.ppm`,
`progress-next-piece-moved.ppm`, `progress-sequence.rgb`, and
`baseline-progress-sequence.rgb`. The earlier
movement/rotation-only profile and its hashes are historical; its 1,320-pixel
difference is not the current progress acceptance check. The latest run's
scripted input differed from its start-only baseline in 407 pixels.

This profile is local and remains outside CI. Its ROM/source checkout and all
save and capture files stay outside the repository. Upstream source and license:
[tbsp/Adjustris](https://github.com/tbsp/Adjustris).

## Automated output checks

For all pinned profiles, success requires a runner summary reporting 3,600
requested/video frames, nonzero delivered audio frames, 160×144 RGB output
of the matching size, and an 8,192-byte SRAM output matching the reported
size. The report includes `validation_error` (null on success). Old RGB,
preview and SRAM outputs are removed before execution so a failed attempt
cannot reuse the preceding frame as fresh evidence. A zero runner exit with
missing or inconsistent artifacts still fails and records the reason.

`make test` runs `tools/test_homebrew_check.py` without external ROMs. It
checks valid output and rejects missing/duplicated summaries, wrong counts,
missing audio, wrong dimensions and truncated/oversized artifacts. It also
checks the Adjustris row-clear and post-clear-control conditions, including
negative cases. A mocked runner verifies that a zero exit without fresh
artifacts cannot pass using stale files. These checks do not establish audible
audio quality or disk-save persistence.

### Big2Small scripted-input control (2026-10-26)

The standard Big2Small profile was compared with two clean, neutral-input runs
using the same pinned ROM and LotusGB core. Both neutral runs produced
identical framebuffers and SRAM. The scripted run differed from the neutral
frame in 18,546 of 23,040 pixels and in 122 SRAM bytes. It completed 3,600
video callbacks, 2,893,139 stereo audio frames, and returned the expected 8 KiB
SRAM block.

| Run | Final RGB SHA-256 | Result |
| --- | --- | --- |
| Neutral A | `d30f1a0b708700255e5edcedb359b4a7cd6a8ab23b7be731eec5a405ab2c24c3` | Identical to Neutral B |
| Neutral B | `d30f1a0b708700255e5edcedb359b4a7cd6a8ab23b7be731eec5a405ab2c24c3` | Identical to Neutral A |
| Scripted input | `be194b9401da6493cd8a631d5f59cb437726b9ff9c291fe2bf9b59c36a3ef11e` | Differs from neutral in 18,546 pixels |

This shows that the pinned scripted profile changes visible and saved game
state for this ROM/core pair. It does not establish each button's individual
mapping, level completion, save-file persistence, or VBlank-boundary behavior.
At this historical 2026-10-27 checkpoint, `make test-homebrew` checked only
callback and output contracts. The neutral comparison was a separate local
diagnostic; the current save/resume acceptance is documented above. The
side-by-side image and comparison JSON are under
`build/test-roms/big2small-input-control/`.

### Three-profile regression after CPU error reporting (2026-10-26)

After unsupported base opcodes were changed to fail explicitly, all three
pinned profiles completed 3,600 frames with 3,600 video callbacks, 2,893,139
stereo audio frames, 8 KiB of generated SRAM, and no runner validation error.
The inspected final captures were nonblank. Core SHA-256:
`b24d849c4a8b64d995c3ec860f2d37602d0ce04bca80190c6c43ccb9fbbc30dd`.

| Profile | Final RGB SHA-256 |
| --- | --- |
| Big2Small | `be194b9401da6493cd8a631d5f59cb437726b9ff9c291fe2bf9b59c36a3ef11e` |
| Tobu Tobu Girl | `260f1d3926acb5a2db9b2ec757b6a493158dc4f9de3b98b7e939df484e35c8f4` |
| Canyon Racer | `8b2cba922ae41dcb3e9cfb107b37cbb0dc870e687acc1da0226c2735d73cd313` |

Tobu Tobu Girl and Canyon Racer match their previously recorded final-frame
hashes. The Big2Small run used no restored player save; its generated SRAM is
not evidence of RetroArch disk persistence. This regression checks that these
scripts still reach stable output after the CPU error-path change, not full
gameplay or hardware accuracy. Reports and captures are under
`build/test-roms/{big2small,tobutobugirl,canyon-racer}/`.

## Canyon Racer raster-effect gameplay check

```sh
make test-homebrew HOMEBREW_GAME=canyon-racer
```

Official source/download: [Canyon Racer by bbbbbr](https://bbbbbr.itch.io/canyon-racer).
Use version 0.7.7's standard `canyon_racer_0.7.7_mbc5.gb`, downloaded to
`../test-roms/homebrew/canyon-racer/`. The pinned ROM SHA-256 is
`19f57c1fefe8ebdcef0ff9def783246a6618d4b822b5c44188a1e9c1e7448c51`
(32 KiB, type `$1B`, MBC5 + RAM + battery, 8 KiB SRAM). The official ZIP
SHA-256 is `f913eae23b43bfc87cb3fa44863ed7e97eb77d4356540d69d7656cc6d8ae5d5b`.
ROM, archive and mixed-license game assets remain outside version control.

The clean-SRAM profile lets the credits/title settle, presses START at frame
900, dismisses first-run help with A at frame 1500, then holds A for 30 frames
every 90 frames from 1800 onward. Outputs use `build/test-roms/canyon-racer/`.
This profile targets the selected release and clean startup; restored progress
can alter the initial help sequence. It does not require winning the game.

On 2026-10-24, intermediate captures at 1800/2400/3600 frames showed the ship,
obstacles, HUD and changing canyon scene. The 3,600-frame run completed with
3,600 video callbacks, 2,893,139 stereo audio frames and an 8 KiB SRAM output.
These are execution/visible-progress observations, not exact hardware rendering
or sound-quality acceptance. The game is difficult and the scripted run loses
lives; this is not a full-level gameplay assertion.

On 2026-10-26, two 3,600-frame runs with the same ROM, core and input script
produced byte-identical final frames. The framebuffer SHA-256 was
`8b2cba922ae41dcb3e9cfb107b37cbb0dc870e687acc1da0226c2735d73cd313`; ROM,
core and input-script SHA-256 values were
`19f57c1fefe8ebdcef0ff9def783246a6618d4b822b5c44188a1e9c1e7448c51`,
`80ec278b8906ac60ce3f865098cec05c50bddef291461aff5cb5a4963fa1b006`, and
`b0e199174963c1fae245255a3c1a9af3a289d1de09355d81eefb2cdab8cb5796`.
This shows repeatability for this profile and build, not hardware accuracy.
The 407-pixel difference recorded in the validation matrix was between
separate historical captures; those records do not retain enough detail to
attribute that difference to one cause.

### Author-described raster-effect check (2026-10-26)

The [developer's notes](https://github.com/bbbbbr/canyon-racer#about-the-effects)
describe vertical parallax from per-scanline, mid-scanline SCY updates, with
different horizontal regions using different vertical offsets; an X-axis sine
wave also distorts the canyon. The author notes that the effect is sensitive
to instruction timing and can show artifacts, so an exact cross-emulator frame
is not a useful acceptance target.

The current core was rerun with `make test-homebrew HOMEBREW_GAME=canyon-racer`
using the pinned 0.7.7 ROM and fixed input profile. It delivered 3,600 of 3,600
video frames and 2,893,139 audio frames. The final frame hash remains
`8b2cba922ae41dcb3e9cfb107b37cbb0dc870e687acc1da0226c2735d73cd313` with core
hash `b24d849c4a8b64d995c3ec860f2d37602d0ce04bca80190c6c43ccb9fbbc30dd`.
Visual inspection of `build/test-roms/canyon-racer/preview.png` shows a
playable canyon scene with the layered scrolling effect described by the
author. This is a repeatable game-level compatibility observation, not a
hardware-timing or pixel-accuracy result; the local runner still requires
manual visual inspection.

A temporary build of `c13d0f0` (before separate SCY bitplane sampling) was run
with the same startup/input sequence. At frame 1800 it differed from the
current frame by 3,463 pixels, mainly canyon texture/row positions. Both
images were inspected. Artifacts are under `build/canyon-check/`, including
`scy-comparison.png` (old left/new right) and its JSON metadata. This demonstrates
the actual game's sensitivity to SCY sampling; it does not establish that
every current pixel is correct.

### Three-core raster diagnostic (2026-10-25)

LotusGB, SameBoy and mGBA completed the same 3,600-frame Canyon Racer input
profile and produced identical 8 KiB SRAM contents. SameBoy and mGBA each
produced byte-identical final frames on repeat runs. After mapping mGBA's four
output colors to the corresponding DMG shade levels, LotusGB differs from
SameBoy in 4,006 pixels and from mGBA in 5,426 pixels; SameBoy and mGBA differ
in 2,980 pixels. The broad gameplay scene is the same, but the raster output
differs across all three implementations.

The common SRAM SHA-256 is
`9df428893828811063e37b31114bbd24a4977d97cd4c72797a5bebb1a48bbc9e`. The ROM
SHA-256 is
`19f57c1fefe8ebdcef0ff9def783246a6618d4b822b5c44188a1e9c1e7448c51`; input
script SHA-256 is
`b0e199174963c1fae245255a3c1a9af3a289d1de09355d81eefb2cdab8cb5796`. The
SameBoy, mGBA and LotusGB core SHA-256 values are, respectively,
`5f3cbc610d1de2c04d41319b6678bece8c5bc78b1067436e1f6d9d440b89b56e`,
`c840f305825d07c9bec7a6ec18841073c4b539fd234f73623474f97c507f1cdc`, and
`33ea67640003f33ef447ecfe9607cda63206fd74a30af36451df0ade234afc44`.
Normalized SameBoy, mGBA and LotusGB frame SHA-256 values are
`d4978737e5cafbcb6dfc601f0f27958f687556929dc6a2c683d740ffb50a9fa6`,
`fb51bb29082af55853fb9bb071381c42e2f746a78938401666badbe82b9b2762`, and
`bc0b5fec05a88c49a82959933122dc1993a90e78678295583dfac4951dc39c22`.

Artifacts and a machine-readable report are under
`build/test-roms/canyon-racer-crosscore/`, including the side-by-side image and
per-core difference maps. mGBA requested RGB565, so this local comparison used
a temporary frontend runner that accepts 0RGB1555/RGB565 as well as XRGB8888;
its source is kept with the ignored local artifacts and was not added to the
project test tooling. mGBA's observed palette levels were
mapped as `(0,0,0) → (0,0,0)`, `(82,81,82) → (85,85,85)`,
`(173,170,173) → (170,170,170)`, and `(255,251,255) → (255,255,255)` before
counting pixels.

This is cross-implementation evidence that the runs reached comparable game
progress, not hardware ground truth. No expected screenshot from a physical
DMG is available, so keep these differences diagnostic rather than treating
them as a pass/fail accuracy score.
