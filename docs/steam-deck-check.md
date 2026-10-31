# Steam Deck acceptance checks

## M6 rewind checkpoint — user-reported pass (2026-10-31)

The user reports that enabling rewind on Deck allows R to rewind Ping Pong,
and that normal control resumes after release. This completes the bounded M6
rewind/release/control checkpoint by user report. SteamOS/RetroArch versions,
installed core hash, exact ROM file/hash, repeated-use duration and measured
FPS/audio behavior were not separately recorded. No core binary changed.
See the [Mac and Deck rewind evidence](rewind.md) for the result, information-
file update requirement and reproduction steps. The earlier M5 result below
verifies manual save/load separately.

## M5 save-state checkpoint — user-reported pass (2026-10-31)

The user reports that both same-session restore/control and restore/control
after a full RetroArch relaunch work normally on Steam Deck. The current
session also shows no screen tearing. This completes the bounded M5 device
checkpoint by user report. SteamOS/RetroArch versions, installed core path and
installed binary hash were not supplied; the candidate identity below is the
delivered artifact, not an independently confirmed loaded-core identity.

The macOS M5 frontend check passed. The delivered Linux candidate is
`build/steam-deck-m5/lotusgb_libretro.so`, cross-compiled with the existing
Zig 0.17.0 toolchain for `x86_64-linux-gnu`. Its SHA-256 is
`6cb1d0057f33ca5fe6c178a2b88840ca59ab17d2e5cc8638726d7e4c537f430a`.
ELF inspection confirms x86-64 and exported `retro_serialize_size`,
`retro_serialize` and `retro_unserialize` symbols. Embedded GLIBC version
strings range from 2.2.5 to 2.17. These build checks do not establish that
the candidate loads or works on the device.

`build/steam-deck-m5.zip` contains this core, its `.info`, the MIT-licensed
GB Pong test copy `lotus-state-smoke.gb`, its license and source attribution,
and build identity/check instructions. The earlier M3 core under
`build/steam-deck/` is retained unchanged; it predates M5.

1. Close content and retain the previous core as a backup. In the tested Deck
   setup, install the updated `.so` again through RetroArch's GUI core-install
   action. Replacing the file at the original installation source path alone
   does not update the installed core, as observed by the user. Fully exit and
   relaunch RetroArch before testing the update. Confirm the installed core's
   hash with `sha256sum /path/to/installed/lotusgb_libretro.so` and record
   SteamOS and RetroArch versions.
2. Load `lotus-state-smoke.gb` with LotusGB. Up/Down should move the left
   paddle. Use a fresh state slot and **Quick Menu → Save State** during play;
   note the paddle/ball position or score and confirm the save notification.
3. Continue until the scene changes. Use **Load State**, confirm the saved
   scene returns, and verify Up/Down still moves the paddle. If paused, resume
   or advance a frame so the restored scene is displayed.
4. Close the content, fully exit and relaunch RetroArch, then load the same
   core and ROM. Select the same slot and load it again. Confirm scene recovery
   and further paddle movement. Report a failed load or crash with the exact
   step and frontend message.
5. Record pass/fail for each stage, the installed core hash, versions, and any
   visual/audio/input problem. A pass closes this bounded device/frontend
   checkpoint; it does not establish all-game compatibility or RTC behavior.

### M5 result record (2026-10-31)

- Same-session save, advance, restore and further control: pass by user report
  (test 3 in the supplied handoff instructions).
- Full RetroArch relaunch, load the same state and further control: pass by
  user report (test 4 in those instructions).
- Screen tearing: user reports no current issue; M4 remains closed for now.
- Core update: user reports GUI core installation must be repeated for the new
  `.so`; updating the original installation source alone is insufficient.
- Candidate: the M5 package identified above. Installed hash/path, frontend and
  SteamOS versions, and explicit ROM identity were not separately reported.

These are manual user observations, not directly captured device results or a
confirmed tearing fix. Earlier M3 battery-save evidence remains separate from
this save-state checkpoint. See the [M5 design and macOS result](save-states.md).

## M3 gameplay and battery-save checkpoint — passed

**Status: five-minute manual checkpoint passed; tearing report closed for now in M4.**
The 2026-10-26 report confirms that an x86-64 LotusGB core loaded in Steam
RetroArch and ran F-1 Race and Pokémon Yellow. It does not identify that older
build's hash or RetroArch version. No direct Steam Deck connection is available
from this task.

The 2026-10-28 user report describes five minutes of normal play, normal-feeling
audio, and working save/load, with occasional screen tearing. On 2026-10-29,
the user confirmed the loaded core's SHA-256 matches the candidate below and
that reopening the game restored its saved progress. The five-minute play-time,
save/reopen, and content reload checks pass. Initially deferred, screen tearing
was briefly prioritized, then closed for now on 2026-10-29 after it did not
recur during a later Deck check and Mac testing showed no tearing. Device/
frontend versions are not recorded; a full
RetroArch process relaunch was not specified.

The standard `build/lotusgb_libretro.dylib` remains an arm64 macOS library. A
separate Steam Deck candidate is now available at
`build/steam-deck/lotusgb_libretro.so`. It was cross-compiled on Apple Silicon
macOS with Zig 0.17.0 for `x86_64-linux-gnu`; `file` identifies it as an ELF
64-bit x86-64 shared object. Its SHA-256 is
`f69f8f6793c60dde35e4d3410f7c3d8b20d4a3b994b02812f5cf766cbb446f34`. ELF
inspection found only glibc 2.17-or-earlier symbol requirements and the usual
glibc, pthread, and dl runtime libraries. The user confirmed that the loaded
core's SHA-256 matches this candidate. Headless host runs do not replace the
device acceptance check.

Rebuild the candidate after extracting the official [Zig 0.17.0 macOS ARM64
archive](https://ziglang.org/download/0.17.0/zig-aarch64-macos-0.17.0.tar.xz)
under `build/toolchains/zig-0.17.0`:

```sh
ZIG_GLOBAL_CACHE_DIR="$PWD/build/zig-cache/global" \
ZIG_LOCAL_CACHE_DIR="$PWD/build/zig-cache/local" \
make -C src/libretro \
  CXX="$PWD/build/toolchains/zig-0.17.0/zig c++ -target x86_64-linux-gnu" \
  build_dir="$PWD/build/steam-deck" \
  "$PWD/build/steam-deck/lotusgb_libretro.so"
```

## Session procedure

Use a licensed homebrew ROM for the reproducible baseline. Big2Small is the
default choice because its first puzzle and SRAM progress have a repeatable
local profile. Keep commercial ROMs and game-derived files private.

1. Record the device's SteamOS version, RetroArch version, LotusGB core path
   and SHA-256, video driver, output resolution/scaling settings, and ROM
   filename/hash. Start a timer when the first content is loaded.
2. Load Big2Small through Steam RetroArch. Check the title screen, enter File A
   and Peanut Pathway, and confirm that directional movement and A/B animal
   selection respond. Continue playing and check music/effects for dropouts,
   distortion, or startup noise.
3. Complete Peanut Pathway with the Elephant's Right, Up, Right solution. Check
   that the completion message appears and the map unlocks Goat Grove. Continue
   playing until the timed session reaches at least 5 minutes; note any
   tearing, scaling problem, missed input, or audio issue and the scene where
   it occurs.
4. Unload the content and return to RetroArch's menu. Load the same ROM again.
   Confirm that File A still shows 02% and Goat Grove remains unlocked. Enter a
   puzzle and verify another directional response. Fully closing and relaunching
   RetroArch provides an additional persistence check.
5. Unload the content once more. Record the final elapsed time and whether
   RetroArch returned to its menu without a crash or stuck screen.

The puzzle completion saves in-game progress to battery SRAM. A successful
core-level SRAM restore does not substitute for the device's `.srm` write and
reload check. Use a disposable test slot and confirm the user's ordinary saves
remain untouched.

## Result record

Current user-reported result. For every issue, include a severity (`blocks
play`, `major`, or `minor`), exact reproduction steps, and whether it repeats
after a restart.

```text
Date/time: Play report 2026-10-28; hash and save-reopen confirmation 2026-10-29
Device / SteamOS version: Not recorded
RetroArch version: Not recorded
LotusGB core path and SHA-256: Candidate hash matched; path not recorded
Video driver / resolution / scaling: Not recorded
ROM filename and SHA-256: Not recorded
Elapsed session time: 5 minutes

Startup and content load: Normal play reported
Controls and input response: No issue reported
Progress and scene transition: Normal play reported; specific transition not recorded
Music and sound effects: Normal by user report
Tearing and scaling on this build: Occasional tearing reported; not seen in the latest Deck check; M4 closed for now
SRAM after game reopen: Pass; full RetroArch relaunch not specified
Content unload/reload: Pass after reopening game

Issues and reproduction steps: Occasional tearing report associated with Pokémon Yellow opening Pikachu lightning; not reproduced in the latest Deck check; Mac testing also shows no tearing; no cause confirmed
Overall M3 result: Pass for bounded checkpoint; M4 tearing report closed for now
```

**M3 passes** when the identified build completes at least five minutes of
play, save/reopen, and content unload/reload without a reproducible failure
that prevents play or loses saved progress. Record any remaining visual/audio
issue for later follow-up. This is evidence for one device, core build,
frontend version, and game, not blanket DMG compatibility or hardware accuracy.

## Screen tearing report — closed for now

The M3 report described occasional screen tearing. The user later reported no
tearing during a Deck check or Mac testing. No cause or core defect was
confirmed, and this does not prove that a Steam Deck setting caused the
original report.
The new [callback-frame regression](dmg-validation.md#frame-coherence-at-the-libretro-callback-boundary-2026-10-29)
checks images delivered to a frontend and detects the earlier mixed-frame
submission bug in a negative control. Its passing synthetic case does not
establish the cause of the current game-specific report.

The user identified Pokémon Yellow's opening Pikachu lightning animation on
2026-10-29. The user later reported no tearing during the Deck check and no
tearing during Mac testing. No production core or Deck binary changed during
the investigation. See the [scene investigation](dmg-validation.md#yellow-intro-lightning-investigation-2026-10-29).

If it returns, record the Deck model, SteamOS and RetroArch
versions, video driver, display refresh/synchronization settings, ROM and exact
scene. Note whether the tear repeats at the same scanline. If practical, compare
a RetroArch frame capture with the physical display, and vary one
synchronization setting per run. Keep PPU timing unchanged until the submitted
frame itself is shown to contain the tear.

Use this order for a small A/B check:

1. Record the current settings and reproduce the same scene once.
2. Check that RetroArch VSync is enabled and its configured vertical refresh
   rate matches the display mode selected in SteamOS. Change only a value that
   is currently wrong, then repeat the scene.
3. If Threaded Video is enabled, turn it off as a separate trial and repeat.
   Restore the original setting if it makes pacing or performance worse.

Libretro's [VSync guide](https://docs.libretro.com/guides/optimal-vsync/)
recommends confirming VSync and the configured display refresh rate; it also
notes that Threaded Video can disrupt frame pacing. Its
[TV guide](https://docs.libretro.com/guides/tv/) describes RetroArch's video
synchronization settings for reducing tearing and stutter. Change one setting
at a time so the comparison identifies which layer affects the symptom.
