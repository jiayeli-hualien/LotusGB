# Emerald RetroArch bridge

`nci.py` sends a small allowlist of read/status and frontend-control commands
to a local RetroArch instance. `input.py` sends one short game-button press
through the host operating system. Neither tool interprets screenshots or
writes game memory.

## Enable NCI for an isolated test instance

In RetroArch, enable **Settings → Network → Network Commands**. The default
UDP port is `55355`; use a different port for each RetroArch instance on the
same machine. Keep the NCI listener reachable only from the local machine or
the explicitly isolated test network. The tool itself defaults to
`127.0.0.1`.

On the tested RetroArch 1.22.2 builds, `GET_STATUS` crashed the Mac frontend
and timed out on the Ubuntu VM. The `observe` commands therefore capture a
screenshot without requesting status. The explicit `status`, `pause`, and
`resume` commands still use `GET_STATUS`; avoid them on those builds.

For a separate test instance, [retroarch-test-override.cfg](retroarch-test-override.cfg)
enables NCI on port `55356` and redirects screenshots, saves, states, playlists,
logs, and core options to `/tmp/emerald-agent-test/`. Use it as an appended
config; do not edit the active RetroArch config for a live netplay session.

The ARM Ubuntu VM preflight passed with this override using Wayland GL, udev
input, audio disabled, and vsync disabled. Start it in the existing graphical
session with `WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=/run/user/1000`,
`LIBGL_ALWAYS_SOFTWARE=1`, and `GALLIUM_DRIVER=llvmpipe`. The synchronous
software-rendered profile can use substantial CPU; use it for short checks.
RetroArch stores this core's save under
`/tmp/emerald-agent-test/saves/LotusGBA/`, so place a disposable save copy
there before launching if a saved checkpoint is required.

## Commands

```sh
python3 tools/emerald_agent/nci.py status
python3 tools/emerald_agent/nci.py screenshot --directory /path/to/retroarch/screenshots
python3 tools/emerald_agent/nci.py observe --directory /path/to/retroarch/screenshots
python3 tools/emerald_agent/nci.py --port 55356 frameadvance --count 3
python3 tools/emerald_agent/nci.py --port 55356 read-memory 02000000 16
python3 tools/emerald_agent/input.py up --duration-ms 180
python3 tools/emerald_agent/input.py a --duration-ms 120
python3 tools/emerald_agent/agent.py --player host --screenshot-dir /path/to/screenshots observe
python3 tools/emerald_agent/agent.py --player host --screenshot-dir /path/to/screenshots act up --duration-ms 180 --ydotool-socket /tmp/lotusgba-ydotool.sock
```

With `--directory`, the screenshot command waits for a new or updated image and
returns its path. Set that argument to the screenshot directory configured in
the isolated RetroArch instance. Without it, the tool reports that the command
was sent; NCI's screenshot action has no documented reply. `frameadvance` sends
one frame-advance command per requested frame, spaced by 20 ms by default, and
expects the core to be paused. Memory reads require an address exposed by the
active core. LotusGBA exposes GBA EWRAM at `0x02000000` (256 KiB, mirrored
through `0x02FFFFFF`) and IWRAM at `0x03000000` (32 KiB, mirrored through
`0x03FFFFFF`) to RetroArch's `READ_CORE_MEMORY` command. For example,
`read-memory 02000000 16` reads the first 16 EWRAM bytes and
`read-memory 03000000 16` reads the first 16 IWRAM bytes.
`RETRO_MEMORY_SYSTEM_RAM` refers to EWRAM; IWRAM is exposed by its GBA bus
address. These reads return raw bytes; interpreting them as player coordinates
or map state still requires game-specific memory addresses and decoding.

## Pokémon Emerald RAM map

`emerald_map.py` reads a full EWRAM/IWRAM snapshot through NCI and discovers
the active SaveBlock1, map-grid buffer, live object events, and player movement
state. It uses the BPEE v1.0 profile generated from `pret/pokeemerald` map and
layout data. Check the ROM identity when its file is available:

```sh
python3 tools/emerald_agent/emerald_map.py --port 55356 \
  --rom-file /path/to/PokemonEmeraldUE.gba
python3 tools/emerald_agent/emerald_map.py --full-map --json
```

Run the decoder on the same machine as RetroArch when NCI is restricted to
localhost, as in the isolated test configuration.

By default it prints a local area around the player. The map uses `#` for a
blocked grid cell, `.` for a cell with no collision bits, `D` for a static warp,
`N` for a live object/NPC, and `P` for the player. The actor list includes
profile labels, local IDs, coordinates, facing, elevation, and movement status.
Text output also includes a hexadecimal elevation overlay; JSON returns the
same values as a numeric grid. This is a navigation-oriented approximation: it
currently does not decode metatile art, direction-specific collision rules,
dialogue, menu text/selection, or link state. For maps using generated layouts
such as the Battle Frontier's special floors, dynamic layouts may not match the
static profile and decoding can fail with a clear message.

Regenerate the checked-in profile from a matching decomp checkout with:

```sh
python3 tools/emerald_agent/generate_map_profile.py /path/to/pokeemerald
```

The button mapping matches the isolated test configs: `a` sends X, `b` sends Z,
directions send the arrow keys, `start` sends Enter, and `select` sends Right
Shift. Linux uses `ydotool` and requires its input daemon/socket to be
available; `--ydotool-socket` selects a non-default socket path. macOS posts
CoreGraphics keyboard events and may
require Accessibility/Input Monitoring permission for the terminal running the
tool. The intended RetroArch window must receive the input; on macOS, make it
the focused app first. Presses are timed in wall-clock milliseconds, not guest
frames.

## Screenshot preprocessing and OCR

`screen_ocr.py` converts a screenshot to grayscale or a black/white binary
image, enlarges the pixel text with nearest-neighbor scaling, then passes that
processed image to Tesseract. It prints recognized text by default; `--json`
also returns line bounding boxes in original screenshot coordinates and mean
word confidence. `--crop x,y,width,height` can isolate a dialogue box or menu;
reported boxes still use coordinates from the full screenshot. `--output-image`
saves the exact processed image sent to OCR.

Install the Python packages in a virtual environment and install the Tesseract
engine separately (Python `pytesseract` is its wrapper):

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r tools/emerald_agent/requirements-ocr.txt
# macOS: brew install tesseract
# Ubuntu: sudo apt install tesseract-ocr
python tools/emerald_agent/screen_ocr.py /path/to/screenshot.png \
  --mode binary --threshold 160 --output-image /tmp/screenshot-binary.png --json
```

Grayscale is the default; use `--mode binary` to test thresholded output and
adjust `--threshold`. Increase `--scale` for small menu text; in the local game
check, scale 4 read the Yes/No labels better than scale 3. Add
`--crop x,y,width,height` with the dialogue/menu bounds when background
graphics add OCR noise. English is the default OCR language (`--lang eng`).
Install matching Tesseract language data before using another language. OCR is
a visual text aid: it does not determine which menu item is selected unless
that selection marker is visible and recognized.

Use short presses and observe the game after each action. On the ARM VM profile,
a 120 ms Right press moved one map tile, while menu navigation worked best with
a 35 ms pulse; longer holds repeated and skipped menu items. Observe the game
after every menu action. Start and Select are mapped from the RetroArch config
but have not been verified in-game. The memory map formatter is read-only.
