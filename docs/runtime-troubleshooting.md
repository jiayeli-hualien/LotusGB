# Runtime troubleshooting

Prioritize reproducible gameplay failures over adding unrelated test coverage.
Separate core execution, the game's own pause state, frontend pause and host
video/audio before changing emulation behavior. Private ROMs and saves stay
outside the repository; record procedures and observations, not their data.

## Rapid pause report (2026-10-24)

The user reported a possible freeze after rapidly pressing pause in Tetris
under WSL RetroArch. The application was already closed, so the pressed key,
menu responsiveness and exact screen could not be confirmed. This report
remains **unreproduced**, not fixed or attributed to WSL.

Local diagnosis used the supplied 32 KiB ROM through the headless libretro
runner. START was held at frames 300–304, 600–604 and 900–904 to reach gameplay.
Between zero-based frames 1500 and 2499, one-frame START pulses were sent
at intervals of 2, 4, 8 or 16 frames. A baseline omitted the burst.

- All five 3,600-frame runs completed with 3,600 video callbacks and
  2,893,139 stereo audio frames; gameplay and pause screens were inspected.
- A temporary system probe extended each sequence to 4,200 frames and
  required every `stepFrame()` to succeed and complete. All reached
  294,940,800 emulated T-cycles.
- At 2/4-frame intervals the final state was running. At 8/16-frame intervals
  the inspected screen said `HIT START TO CONTINUE GAME`: these sequences
  contain an odd number of pause toggles and ended paused.
- For the latter sequences, one extra START hold at frames 2800–2804 resumed
  play. Four sampled paused frames shared a hash; all fourteen subsequent
  samples (every 100 frames, 2900–4200) differed. This checks resumed visual
  progress, beyond merely receiving frontend callbacks.

These bounded sequences did not reproduce a stuck core. They do not exclude
other input phases, longer sessions, frontend problems or the reported bug.
Local artifacts are under `build/tetris-pause/`; no private ROM, player save
or commercial game data was added to version control.

The inspected local RetroArch configuration maps Enter to game START, P to
frontend pause and F1 to the menu, with pause-on-focus-loss enabled. These
are observations of that installation, not universal key defaults; no
configuration was modified. If it occurs again, retain the frame and key
pressed, try releasing all keys and pressing the same pause key once, and
check whether the frontend menu still opens. A responsive menu plus a stuck
game calls for a core trace; an unresponsive frontend needs frontend/host
logs before blaming the emulator.
