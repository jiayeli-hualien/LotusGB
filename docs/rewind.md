# RetroArch rewind acceptance

The bounded macOS rewind checkpoint passed on 2026-10-31 with GB Pong.
Holding Rewind returned the score and paddle position to earlier values;
releasing it allowed continued play and directional control. On Steam Deck,
the user reports that R rewinds Ping Pong and normal control resumes after
release. This completes the bounded M6 frontend checkpoint by user report.
No core code or binary changed.

## Setup and core information

RetroArch implements the rewind history using the core's serialization
callbacks. LotusGB supplies the versioned [M5 snapshots](save-states.md).
Rewind is a separate acceptance target from manual Save State / Load State.

Update both the core library and `lotusgb_libretro.info`. In the tested Mac
installation the installed info file still contained `savestate = "false"`;
RetroArch rejected rewind even when the current library was loaded. The
repository's info file contains `savestate = "true"`. Point RetroArch at that
file or copy it into the configured **Core Info** directory, then fully
restart the frontend. Its configured path is `libretro_info_path`. A cached
old entry may need a core-info cache refresh. Do not enable the save-state
feature bypass to conceal an outdated info file.

The existing macOS `make install-retroarch-core-macos` target copies only the
library; it does not install the info file. On the user's Deck setup, updating
the core also requires repeating GUI core installation; changing its original
source file alone is insufficient. Check the info directory separately.

For this check, enable **Rewind Support** in RetroArch's rewind settings
(**Settings → Frame Throttle → Rewind**, or the Quick Menu rewind page),
use granularity 1 and a 20 MiB buffer, and bind **Rewind** under
**Settings → Input → Hotkeys**. Hold the binding during gameplay and release
it to resume. Menu labels can vary with language and frontend version.

## macOS result (2026-10-31)

| Identity / setting | Value |
| --- | --- |
| Frontend | `/Applications/RetroArch.app`, 1.22.2, Git `69a4f0ea` |
| Platform | Apple M1, arm64 macOS 27.0.1 (26A434) |
| Core | `build/lotusgb_libretro.dylib`, SHA-256 `93d9e02e5bce9ae0ea2bbec87b4f8dc9ba11bbb948d6ec7dcfe8cefe53f8f842` |
| ROM | `lotus-state-smoke.gb`, licensed GB Pong copy, SHA-256 `9c5e337db0ee1416f2c7f1de02e7e0a78a6064cf1da49209b4bc1a36a7b35415` |
| Startup / video | Fast startup without BIOS; GL, XRGB8888 |
| Rewind | Enabled, 20 MiB buffer, granularity 1, keyboard `r`, bypass disabled |

The successful run used a separate config, save/state directories and current
core-info directory under `build/test-roms/rewind/macos-2026-10-31/`.
`config_save_on_exit` was disabled, leaving ordinary frontend settings intact.
RetroArch's log confirms rewind buffer initialization. The session used its
CLI to select the exact library, ROM and config, and Computer Use for gameplay
input and screenshots.

1. Moved the left paddle up, then down. Before rewind, the score was 3–3 and
   the paddle occupied screenshot Y=1106–1425.
2. Held `r` for ten seconds. The next capture showed 2–3 and the paddle at
   Y=266–585: both returned to earlier values.
3. Released `r` and held Down. The paddle returned to Y=1106–1425 while the
   game continued scoring and animating.
4. Repeated a six-second rewind and continued play. Further focused Up input
   moved the paddle from the bottom to Y=806–1125. The run stayed active and
   ended through the normal Quit menu.

The FPS overlay showed 60.36, 60.05, 60.36 and 59.73 in selected captures.
These short frontend samples show no sustained slowdown in this session;
they are not a benchmark or precise core timing measurement. The successful
run's log contained no errors; its stock-shader warning is unrelated to rewind.
Audio quality was not assessed. Running-RTC behavior, long sessions, other
games, run-ahead and replay remain unvalidated.

Screenshots `07-repeat-down.png`, `08-repeat-rewound.png`,
`09-repeat-resumed-down.png` and `13-focused-resumed-up.png`, plus
`retroarch-rewind-repeat.log` and `report.json`, retain the local evidence in
the ignored directory above. Screenshots are samples after key release,
not exact-frame replay comparisons.

Earlier attempts are not passing evidence: native file-dialog automation
timed out, and one mixed menu/keyboard run ended with process exit status 0
before a valid rewind capture. Its cause was not established; the later
keyboard-only run with two rewind holds did not reproduce the exit. The
installed old info file also produced an explicit rewind-not-supported error
before the isolated info path was corrected. That bounded checkpoint did not
include a core performance fix.

## macOS Steam audio follow-up (2026-11-02)

The user reported crackling during forward playback with rewind enabled. The
Steam RetroArch configuration uses a separate core directory from the
standalone macOS configuration, so the first package installation updated the
other copy. After installing the updated core through the Steam config, the
user reports clean audio with rewind enabled during normal play on Mac Steam
RetroArch, and then on Steam Deck. The updated core caches the immutable ROM
identity instead of hashing the whole ROM for every snapshot. The user also
confirmed normal audio while holding Rewind and after releasing it. These are
user-reported listening checks; the Deck core hash and session duration were
not recorded. RetroArch has a separate `audio_rewind_mute` setting for muting
while Rewind is active; the inspected Mac configs set it to `false`, so they
do not request that mute behavior. The Linux package includes the same
optimization.

## Steam Deck result — user-reported pass (2026-10-31)

On 2026-10-31 the user reported that, after enabling rewind on Steam Deck,
pressing R rewinds successfully. The follow-up confirms normal control after
release and identifies the tested game as Ping Pong. This passes the bounded
rewind/release/control checkpoint. The exact ROM file/hash was not supplied,
so its identity is retained as reported rather than equated to the Mac test
copy. Installed core hash, SteamOS/RetroArch versions, repeated-use duration,
FPS and audio quality were not separately recorded. The result does not claim
long-session stability, RTC rewind or other-game compatibility.

Use the same Linux core and GB Pong from `build/steam-deck-m5.zip`; a new
library build is unnecessary. Ensure the package's current `.info` is in
RetroArch's configured Core Info directory, then restart RetroArch. If the
current M5 library is already installed, no library reinstall is required.

1. Record RetroArch/SteamOS versions and the installed core hash. The delivered
   Linux SHA-256 is `6cb1d0057f33ca5fe6c178a2b88840ca59ab17d2e5cc8638726d7e4c537f430a`.
2. Enable rewind with granularity 1 and a 20 MiB buffer. Assign an unused
   controller hotkey/button to Rewind; note any Hotkey Enable modifier.
3. Play GB Pong for 10–20 seconds, moving the paddle and noting a score or
   position. Hold Rewind for 3–5 seconds and verify visible regression.
4. Release it and confirm Up/Down still controls the paddle. Repeat once.
   Note FPS, audio issues, freezes, crashes or frontend messages. Disable
   rewind afterwards if it is not desired for ordinary play.

The procedure above remains available for reproduction or another build.
This device checkpoint is separate from M5 save/load.

## Frontend references

- [Libretro serialization and frontend uses](https://docs.libretro.com/development/cores/developing-cores/)
- [RetroArch 1.22.2 core-info save-state feature parsing](https://github.com/libretro/RetroArch/blob/v1.22.2/core_info.c#L1723)
