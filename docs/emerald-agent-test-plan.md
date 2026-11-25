# Pokémon Emerald link battle agent test plan

## Goal

Automate the in-game steps that start a Pokémon Emerald link battle, especially
walking to the link-room attendant, talking to the NPC, navigating the game
menus, and confirming that both players reach the same link-battle stage. The
first milestone is reliable arrival at the expected battle lobby on both
instances; battle completion and save persistence are later milestones.

Keep the game ROM and saves unchanged. Run each endpoint from its own disposable
save copy and keep captured game-derived artifacts outside public source control.

## Test setup

Use two RetroArch instances, one per player, each with its own LotusGBA core,
configuration, screenshot directory, log, and save copy. Record for each side:

- RetroArch and core version, core binary hash, and core options;
- ROM identity/hash and save/RTC copy identity;
- host/client role, selected link device, NCI port, and input backend;
- starting game checkpoint, player position, and visible game state.

Use a prepared checkpoint near the relevant Pokémon Center link-room path
for the first navigation trial. This keeps the test focused on NPC navigation
and menu handling rather than unrelated game progression. Later, test from a
more distant checkpoint if needed.

### ARM VM preflight (2026-11-24)

The selected target is the Ubuntu ARM VM, and `uname -m` reports `aarch64`.
Its installed RetroArch executable and LotusGBA core are both ARM64 binaries.
A temporary test copy of the ROM and save was used with
NCI port `55356` and output directories under `/tmp/emerald-agent-test/`.

The working short-run profile uses `video_driver = "gl"`,
`video_context_driver = "wayland"`, `video_vsync = "false"`,
`audio_enable = "false"`, and `input_driver = "udev"`, with
`WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=/run/user/1000`,
`LIBGL_ALWAYS_SOFTWARE=1`, and `GALLIUM_DRIVER=llvmpipe`. With the display
loop unblocked, NCI status and screenshot capture both passed. The initial
title-menu check verified A, Down, and Up through a private ydotool socket; the
later navigation results are recorded below.

RetroArch redirects the save into a core-specific subdirectory, so the
disposable save must be at
`/tmp/emerald-agent-test/saves/LotusGBA/PokemonEmeraldUE.srm`. The initial
copy was one directory too high and the core skipped it. After correcting the
path, Continue loaded the prepared checkpoint in Petalburg City's Pokémon
Wireless Club Direct Corner. This save already starts at the relevant link-room
attendant and does not need a route from the overworld.

The one-instance navigation and link-entry smoke check covered these steps:

1. A opened Continue; A loaded the save.
2. One short Right press moved one tile toward the right-side attendant. Up
   faced the attendant, and A opened dialogue. The text identified the room as
   `POKéMON WIRELESS CLUB DIRECT CORNER`.
3. A advanced to the `TRADE CENTER` / `COLOSSEUM` / `EXIT` menu. Down selected
   Colosseum; A opened `SINGLE BATTLE` / `DOUBLE BATTLE` / `MULTI BATTLE` /
   `INFO` / `EXIT`.
4. A selected Single Battle. The game requested a save and then an overwrite
   confirmation. Both were confirmed on the disposable copy only.
5. The next menu offered `JOIN GROUP`, `BECOME LEADER`, and `EXIT`. Selecting
   Become Leader showed `Please do visit again.` and the captured game screen
   then went black. NCI still reported the core as playing. The one-instance
   run did not produce a legible wait/lobby screen, so the black screen could
   not be distinguished from an expected wait state or a link/core problem.
   No second game instance was present, so no two-player handshake or battle
   lobby was verified.

The screenshot supports this partial text map: three attendants and service
counters line the far side of the room, with connection terminals in front of
them; the player starts on the open central floor beside the middle terminal.
One tile Right aligns the player with the right-side attendant; Up faces the
counter, and A opens its dialogue. This is a route-level description, not yet a
tile-perfect passability map.

On this profile, a 120 ms Right press produced one tile of map movement. For
the role menu, 120 ms Down did not move the cursor consistently, 300 ms Down
skipped two items, and a 35 ms pulse moved exactly one item. Use short pulses
for menus and inspect the resulting screenshot before confirming. These are
wall-clock timings, not guest-frame timings.

The original save and the disposable save had matching SHA-256 values at the
final check.
The active RetroArch config was not changed. The test RetroArch process and
private ydotool daemon were stopped; the VM's pre-existing ydotool daemon was
left running.

### RAM observer navigation smoke test (2026-11-25)

Using a separate temporary RetroArch instance, NCI port `55358`, and a copied
save, the BPEE decoder reported the player at `(9, 4)`, facing south, in
Pokémon Center 2F. A 120 ms Right press moved the player to `(10, 4)` and
reported east; a 120 ms Up press kept the player on that tile and reported
north. A then opened the Direct Corner dialogue, confirmed by screenshot text:
`POKéMON WIRELESS CLUB DIRECT CORNER.` The RAM map identified all three room
attendants and their coordinates before the interaction. This verifies the
single-player route-and-talk loop, not menu handling or a two-endpoint link.

The disposable save hash was unchanged after the run. The temporary RetroArch
window was closed; the pre-existing ydotool daemon was left running.

### Local Mac host attempt (2026-11-24)

The local macOS ARM64 host has RetroArch 1.22.2 and an arm64 LotusGBA core. A
disposable copy of the prepared test save and the same ROM were placed under a
temporary directory; the isolated core option selects
`Wireless Adapter`, and the host was configured for NCI port `55357` and
netplay port `55435`. The host log confirms that LotusGBA started its GBA
netplay transport.

The first Mac run crashed after the NCI `GET_STATUS` request. The macOS report
shows `EXC_BAD_ACCESS` in RetroArch's `command_get_status`, reached through
`command_parse_msg` and `command_network_poll`; the core's input polling is
only the caller path. Do not use NCI status/observe/pause helpers on this Mac
build until this is resolved. A status-free screenshot request has not yet
been checked.

A subsequent launch, including one after the user reopened RetroArch from the
Applications folder, exited before loading content with `SIGABRT` in AppKit's
application-registration path. `open -n` also returned `kLSNoExecutableErr`.
The reopened GUI instance uses the normal profile (`network_cmd_enable=false`
and the default save directory under Documents), so it was left unloaded and
untouched. The next attempt should first quit that normal instance, then start
the isolated profile as the only RetroArch process, avoid `GET_STATUS`, and
check with NCI `SCREENSHOT` only. The VM client was not started, so no
two-endpoint connection was attempted.

Earlier X11 attempts crashed in `XScreenSaverQueryExtension`; the synchronous
Wayland renderer also stalled until vsync was disabled. Keep this llvmpipe
profile to short tests because it can use substantial CPU.

The Wireless Club Direct Corner menu includes `TRADE CENTER`, `COLOSSEUM`, and
`EXIT`. The battle-mode menu includes `SINGLE BATTLE`, `DOUBLE BATTLE`,
`MULTI BATTLE`, `INFO`, and `EXIT`. Identify the visible option and cursor
before each confirmation instead of advancing through several prompts with a
blind sequence of A presses. Selecting a battle mode first saves the game; keep
that operation confined to the disposable save copy.

## Interface responsibilities

RetroArch's Network Control Interface (NCI) can provide frontend actions such
as pause, reset, frame advance, screenshot, and memory reads when the core
publishes a suitable map. It does not provide general gamepad button injection
or interpret Pokémon state. A small external bridge should combine:

1. NCI for frontend control and screenshot capture;
2. a host-specific virtual/keyboard controller backend for game buttons;
3. a game adapter that decodes observations and exposes semantic actions.

Keep one bridge connected to each RetroArch instance. Never assume that an
action or observation on host also describes the client.

## Observation data to extract

### First milestone: read-only RAM map

LotusGBA now exposes GBA EWRAM and IWRAM through RetroArch's
`READ_CORE_MEMORY`. `tools/emerald_agent/emerald_map.py` uses the BPEE v1.0
profile in `tools/emerald_agent/emerald_bpee_v1_map_data.json` to discover the
current save block and live map grid, then formats these fields:

- current map/group identifier and map dimensions;
- loaded metatile grid, collision/passability, elevation, and warp tiles;
- player tile coordinates, facing direction, and movement-in-progress state;
- object-event/NPC coordinates and relevant interaction range;

The decoder reads only; `--rom-file` verifies game code BPEE and ROM
version 0. It emits the local map grid, static warp tiles, live object-event
coordinates/labels, and player position/facing/movement status. Collision is a
coarse passability signal from Emerald's grid bits; directional tile rules,
dialogue/menu state, and link state are not decoded yet. Dynamic special layouts
may need a separate profile. The Ubuntu VM check decoded the Direct Corner
room, player position, collision grid, and its three attendants.

### Follow-up: screenshot text and link state

- Screenshot and capture timestamp from each endpoint;
- visible dialogue text and menu options, preferably with the selected item;
- current stage such as overworld, dialogue, menu, link search, lobby, or battle
  setup;
- link state needed to confirm the lobby/battle transition.

Preprocess screenshots to grayscale or binary before OCR. Preserve recognized
text, coordinates, and confidence so the planner can check which menu item is
selected. Keep the RAM map as the geometry source and compare visible NPC/menu
state against it.

## Normalized observation and action contract

The bridge should return a structured observation per player, with a shape like:

```json
{
  "player": "host",
  "stage": "overworld",
  "map_id": "unknown",
  "position": {"x": 0, "y": 0},
  "facing": "down",
  "room": ["########", "#..N...#", "#..P...#", "####D###"],
  "dialogue": null,
  "menu": null,
  "confidence": {"room": 0.8, "position": 0.7}
}
```

Use low-level actions such as `press(button, duration)` and `release(button)`;
add game-specific actions such as `walk_to_adjacent_tile(NPC)` and
`choose_menu_option("Colosseum")` only in the Emerald adapter. Each action
should be followed by a fresh observation. Long unobserved button holds are
unsafe around NPCs, doorways, and menus.

The initial controller backend may use timed host input. If frame-exact actions
prove necessary, pause the core and combine held input with NCI frame advance,
or add a core-side test-control API. Do not treat elapsed wall-clock time as an
exact number of guest frames.

## Test stages and pass criteria

1. **Observe:** capture both start screens and produce a legible room grid with
   player and target NPC marked. Human-check the map once.
2. **Navigate:** move each player independently to the target attendant, stop
   on an adjacent tile, face the NPC, and verify the dialogue opened.
3. **Select:** read the visible prompt, choose the battle/Colosseum option, and
   wait for the next prompt before confirming. Record each endpoint's screen.
4. **Link setup:** follow the host/client-specific prompts, observing both
   endpoints after each transition. The single-end test reached the
   `JOIN GROUP` / `BECOME LEADER` role menu but did not produce a legible link
   wait screen.
   Pass only when both endpoints reach the expected battle lobby/setup state
   and the core logs show the intended link profile.
5. **Battle smoke check:** if lobby setup passes, continue through team/rules
   selection and verify both games reach the first battle turn.
6. **Repeatability:** repeat from fresh copies at least three times, then swap
   host/client roles. Keep navigation failures separate from link-protocol
   failures.

For each action, retain timestamp, endpoint, button/duration, before/after
observation, screenshot reference, and any NCI/core error. A session is not a
pass merely because RetroArch Netplay connected; both games must reach the
specified in-game stage.

## Implementation order

1. Add the Python NCI helper, isolated configuration, screenshot capture, and
   host-input backend. **Implemented.**
2. Expose LotusGBA EWRAM/IWRAM and build the BPEE v1.0 read-only map observer.
   **Implemented; the Ubuntu VM check decoded the Direct Corner room and all
   three attendants.**
3. Use the map output to route each player to the intended attendant; compare
   the predicted route and interaction tile with screenshots.
4. Implement the Colosseum NPC/menu workflow; the earlier one-instance smoke
   check reached the leader/join role menu. Verify the complete workflow with
   both endpoints running.
5. Decode menu/dialogue/link state only where memory provides stable signals.
6. Add grayscale/binary screenshot preprocessing and OCR for visible dialogue
   and menus; compare OCR selections against memory observations.
7. Add repeatable two-endpoint acceptance runs and evidence summaries.

Do not install cores, alter active RetroArch settings, or touch original saves
as part of developing the bridge. Use isolated test configuration and save
copies for any live run.
