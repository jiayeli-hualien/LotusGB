#!/usr/bin/env python3
"""Read-only Pokémon Emerald RAM decoder for RetroArch's NCI interface."""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from nci import DEFAULT_HOST, DEFAULT_PORT, DEFAULT_TIMEOUT, NciClient


EWRAM_BASE = 0x02000000
EWRAM_SIZE = 256 * 1024
IWRAM_BASE = 0x03000000
IWRAM_SIZE = 32 * 1024
MAX_MAP_DATA_SIZE = 10_240
MAP_OFFSET_W = 15
MAP_OFFSET_H = 14
MAP_OFFSET = 7
SAVE_BLOCK1_LOCATION_OFFSET = 0x04
SAVE_BLOCK1_MAP_LAYOUT_ID_OFFSET = 0x32
OBJECT_EVENTS_OFFSET = 0x0A30
OBJECT_EVENT_COUNT = 16
OBJECT_EVENT_SIZE = 0x24
PLAYER_AVATAR_SIZE = 0x24
MAPGRID_UNDEFINED = 0x03FF
MAPGRID_COLLISION_MASK = 0x0C00
MAPGRID_COLLISION_SHIFT = 10
MAPGRID_ELEVATION_SHIFT = 12

PROFILE_PATH = Path(__file__).with_name("emerald_bpee_v1_map_data.json")


class MemoryReadError(RuntimeError):
    pass


@dataclass(frozen=True)
class SaveBlock:
    offset: int
    map_group: int
    map_num: int
    x: int
    y: int
    layout_id: int


@dataclass(frozen=True)
class BackupMap:
    pointer: int
    width: int
    height: int
    map_width: int
    map_height: int
    score: int


@dataclass(frozen=True)
class Actor:
    local_id: int
    graphics_id: int
    x: int
    y: int
    elevation: int
    facing: int
    movement_direction: int
    moving: bool
    is_player: bool


def read_core_memory(client: NciClient, address: int, length: int) -> bytes:
    """Read a range in bounded NCI requests and validate each response."""
    result = bytearray()
    while len(result) < length:
        count = min(4096, length - len(result))
        current = address + len(result)
        response = client.send(f"READ_CORE_MEMORY {current:X} {count}")
        match = re.match(r"^READ_CORE_MEMORY\s+\S+\s+(.+)$", response)
        if not match:
            raise MemoryReadError(f"unexpected response at 0x{current:08X}: {response}")
        fields = match.group(1).split()
        if not fields or fields[0].startswith("-1"):
            raise MemoryReadError(f"unmapped address 0x{current:08X}: {response}")
        try:
            block = bytes(int(field, 16) for field in fields)
        except ValueError as exc:
            raise MemoryReadError(f"invalid byte data at 0x{current:08X}: {response}") from exc
        if len(block) != count:
            raise MemoryReadError(
                f"short read at 0x{current:08X}: requested {count}, received {len(block)}"
            )
        result.extend(block)
    return bytes(result)


def read_snapshot(client: NciClient) -> tuple[bytes, bytes]:
    # Full snapshots make the pointer discovery independent of build-specific
    # globals in IWRAM and the game's ASLR-shifted save block.
    iwram = read_core_memory(client, IWRAM_BASE, IWRAM_SIZE)
    ewram = read_core_memory(client, EWRAM_BASE, EWRAM_SIZE)
    return iwram, ewram


def pointer_slots(iwram: bytes) -> list[tuple[int, int]]:
    pointers = []
    for offset in range(0, len(iwram) - 3, 4):
        pointer = struct.unpack_from("<I", iwram, offset)[0]
        if EWRAM_BASE <= pointer < EWRAM_BASE + EWRAM_SIZE:
            pointers.append((offset, pointer))
    return pointers


def load_profile() -> dict[str, object]:
    try:
        return json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
    except OSError as exc:
        raise RuntimeError(f"cannot load Emerald map profile: {exc}") from exc


def profile_maps(profile: dict[str, object]) -> list[list[dict[str, object]]]:
    return [group["maps"] for group in profile["groups"]]


def find_backup_map(
    iwram: bytes,
    ewram: bytes,
    pointers: list[tuple[int, int]],
    expected_width: int,
    expected_height: int,
) -> BackupMap:
    candidates: list[BackupMap] = []
    for slot, pointer in pointers:
        if slot < 8:
            continue
        width, height = struct.unpack_from("<ii", iwram, slot - 8)
        if width != expected_width + MAP_OFFSET_W or height != expected_height + MAP_OFFSET_H:
            continue
        cells = width * height
        if cells > MAX_MAP_DATA_SIZE:
            continue
        ewram_offset = pointer - EWRAM_BASE
        byte_count = cells * 2
        if ewram_offset + byte_count > len(ewram):
            continue

        blocks = struct.unpack_from(f"<{cells}H", ewram, ewram_offset)
        mapped = sum((block & 0x03FF) != MAPGRID_UNDEFINED for block in blocks)
        expected_area = expected_width * expected_height
        if expected_area <= 0 or mapped < max(24, expected_area // 2):
            continue
        unique_tiles = len({block & 0x03FF for block in blocks if (block & 0x03FF) != MAPGRID_UNDEFINED})
        if unique_tiles < 2:
            continue

        # Dimensions come from the active map's named Emerald layout, which
        # prevents a random EWRAM pointer from being mistaken for this grid.
        coverage = min(mapped, expected_area * 2)
        score = coverage * 4 + unique_tiles
        candidates.append(
            BackupMap(pointer, width, height, expected_width, expected_height, score)
        )

    if not candidates:
        raise MemoryReadError("could not locate Emerald's live backup map grid in RAM")
    # The same backing pointer may be referenced from more than one IWRAM word.
    candidates = list({candidate.pointer: candidate for candidate in candidates}.values())
    candidates.sort(key=lambda item: item.score, reverse=True)
    best = candidates[0]
    if len(candidates) > 1 and candidates[1].score == best.score:
        raise MemoryReadError("multiple live map grids matched; refusing an ambiguous decode")
    return best


def find_save_block(
    ewram: bytes,
    pointers: list[tuple[int, int]],
    maps: list[list[dict[str, object]]],
) -> SaveBlock:
    candidates: dict[int, tuple[int, SaveBlock]] = {}
    for _, pointer in pointers:
        offset = pointer - EWRAM_BASE
        if offset + 0x3D64 > len(ewram):
            continue
        x, y = struct.unpack_from("<hh", ewram, offset)
        map_group = struct.unpack_from("<b", ewram, offset + SAVE_BLOCK1_LOCATION_OFFSET)[0]
        map_num = struct.unpack_from("<b", ewram, offset + SAVE_BLOCK1_LOCATION_OFFSET + 1)[0]
        if map_group < 0 or map_group >= len(maps):
            continue
        if map_num < 0 or map_num >= len(maps[map_group]):
            continue
        layout_id = struct.unpack_from("<H", ewram, offset + SAVE_BLOCK1_MAP_LAYOUT_ID_OFFSET)[0]
        map_data = maps[map_group][map_num]
        if layout_id != int(map_data["layout_id"]):
            continue
        # The player can stand in connected-map border cells. Allow the
        # runtime's border margin while rejecting unrelated EWRAM structures.
        if not (
            -MAP_OFFSET <= x < int(map_data["layout_width"]) + MAP_OFFSET
            and -MAP_OFFSET <= y < int(map_data["layout_height"]) + MAP_OFFSET
        ):
            continue
        block = SaveBlock(offset, map_group, map_num, x, y, layout_id)
        score = 100
        object_start = offset + OBJECT_EVENTS_OFFSET
        if object_start + OBJECT_EVENT_COUNT * OBJECT_EVENT_SIZE <= len(ewram):
            saved_objects = parse_actors(
                ewram[object_start:object_start + OBJECT_EVENT_COUNT * OBJECT_EVENT_SIZE],
                map_group,
                map_num,
                int(map_data["layout_width"]) + MAP_OFFSET_W,
                int(map_data["layout_height"]) + MAP_OFFSET_H,
            )
            if any(actor.is_player and actor.x == x and actor.y == y for actor in saved_objects):
                score += 25
        previous = candidates.get(offset)
        if previous is None or score > previous[0]:
            candidates[offset] = (score, block)

    if not candidates:
        raise MemoryReadError("could not locate the active Emerald SaveBlock1 pointer")
    ranked = sorted(candidates.values(), key=lambda item: item[0], reverse=True)
    if len(ranked) > 1 and ranked[0][0] == ranked[1][0]:
        raise MemoryReadError("multiple SaveBlock1 candidates matched; refusing an ambiguous decode")
    return ranked[0][1]


def parse_actors(
    data: bytes,
    map_group: int,
    map_num: int,
    map_width: int,
    map_height: int,
) -> list[Actor]:
    actors: list[Actor] = []
    for index in range(min(OBJECT_EVENT_COUNT, len(data) // OBJECT_EVENT_SIZE)):
        offset = index * OBJECT_EVENT_SIZE
        flags = struct.unpack_from("<I", data, offset)[0]
        if not flags & 1:
            continue
        is_player = bool(flags & (1 << 16))
        if data[offset + 9] != map_num or data[offset + 10] != map_group:
            continue
        # Object-event coordinates use the padded map-grid origin; saveblock
        # and map-profile coordinates are map-local and start at (0, 0).
        raw_x, raw_y = struct.unpack_from("<hh", data, offset + 0x10)
        x, y = raw_x - MAP_OFFSET, raw_y - MAP_OFFSET
        if not (0 <= x < map_width and 0 <= y < map_height):
            continue
        directions = struct.unpack_from("<H", data, offset + 0x18)[0]
        actors.append(
            Actor(
                local_id=data[offset + 8],
                graphics_id=data[offset + 5],
                x=x,
                y=y,
                elevation=data[offset + 0x0B] & 0x0F,
                facing=directions & 0x0F,
                movement_direction=(directions >> 4) & 0x0F,
                moving=bool(flags & ((1 << 1) | (1 << 6))),
                is_player=is_player,
            )
        )
    return actors


def find_live_object_events(
    ewram: bytes,
    save: SaveBlock,
    backup_map: BackupMap,
) -> tuple[list[Actor], dict[str, int] | None]:
    array_size = OBJECT_EVENT_COUNT * OBJECT_EVENT_SIZE
    avatar_offset = array_size
    best: tuple[int, list[Actor], dict[str, int] | None] | None = None
    last_start = len(ewram) - array_size - PLAYER_AVATAR_SIZE

    for array_start in range(0, last_start, 4):
        player_index = None
        player_x = player_y = 0
        for index in range(OBJECT_EVENT_COUNT):
            current = array_start + index * OBJECT_EVENT_SIZE
            flags = struct.unpack_from("<I", ewram, current)[0]
            if (flags & 0x10001) != 0x10001:
                continue
            if ewram[current + 8] != 255:
                continue
            if ewram[current + 9] != save.map_num or ewram[current + 10] != save.map_group:
                continue
            candidate_x, candidate_y = struct.unpack_from("<hh", ewram, current + 0x10)
            if (
                abs(candidate_x - (save.x + MAP_OFFSET)) > 1
                or abs(candidate_y - (save.y + MAP_OFFSET)) > 1
            ):
                continue
            player_index = index
            player_x, player_y = candidate_x, candidate_y
            break
        if player_index is None:
            continue

        actors = parse_actors(
            ewram[array_start:array_start + array_size],
            save.map_group,
            save.map_num,
            backup_map.map_width,
            backup_map.map_height,
        )
        if not any(actor.is_player for actor in actors):
            continue
        avatar = ewram[array_start + avatar_offset:array_start + avatar_offset + PLAYER_AVATAR_SIZE]
        avatar_info = None
        score = len(actors)
        if (
            len(avatar) == PLAYER_AVATAR_SIZE
            and avatar[5] == player_index
            and avatar[2] <= 3
            and avatar[3] <= 4
        ):
            avatar_info = {
                "running_state": avatar[2],
                "tile_transition_state": avatar[3],
                "object_event_id": avatar[5],
            }
            score += 1000
        if player_x == save.x + MAP_OFFSET and player_y == save.y + MAP_OFFSET:
            score += 100
        if best is None or score > best[0]:
            best = (score, actors, avatar_info)

    if best is None:
        return [], None
    return best[1], best[2]


def direction_name(value: int) -> str:
    return {1: "south", 2: "north", 3: "west", 4: "east"}.get(value, "unknown")


def readable_name(name: str) -> str:
    for prefix in ("LOCALID_", "OBJ_EVENT_GFX_"):
        if name.startswith(prefix):
            name = name[len(prefix):]
            break
    words = []
    for part in name.split("_"):
        if not part:
            continue
        split = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", part)
        split = re.sub(r"([A-Za-z])(\d+)$", r"\1 \2", split)
        words.append(split)
    return " ".join(words).replace(" 1 F", " 1F").replace(" 2 F", " 2F")


def actor_labels(map_data: dict[str, object]) -> dict[int, dict[str, str]]:
    labels: dict[int, dict[str, str]] = {}
    for index, actor in enumerate(map_data.get("actors", []), start=1):
        local_id_num = int(actor.get("local_id_num", index))
        local_id = actor.get("local_id")
        graphics = actor.get("graphics_id") or "OBJ_EVENT"
        script = actor.get("script")
        label = local_id or graphics
        if script and script != "0x0" and not local_id:
            label = script.rsplit("_", 1)[-1]
        labels[local_id_num] = {"label": readable_name(str(label)), "graphics_id": str(graphics)}
    return labels


def render_observation(
    profile: dict[str, object],
    ewram: bytes,
    backup_map: BackupMap,
    save: SaveBlock,
    actors: list[Actor],
    avatar: dict[str, int] | None,
    radius: int | None,
) -> dict[str, object]:
    maps = profile_maps(profile)
    map_data = maps[save.map_group][save.map_num]
    grid_offset = backup_map.pointer - EWRAM_BASE
    player_actor = next((actor for actor in actors if actor.is_player), None)
    player_x = player_actor.x if player_actor else save.x
    player_y = player_actor.y if player_actor else save.y
    actor_by_xy: dict[tuple[int, int], list[Actor]] = {}
    for actor in actors:
        if not actor.is_player:
            actor_by_xy.setdefault((actor.x, actor.y), []).append(actor)
    warp_positions = {
        (int(warp["x"]), int(warp["y"])) for warp in map_data.get("warps", [])
    }

    if radius is None or (backup_map.map_width <= 2 * radius + 1 and backup_map.map_height <= 2 * radius + 1):
        left, top = 0, 0
        right, bottom = backup_map.map_width, backup_map.map_height
    else:
        extent = radius
        left = max(0, player_x - extent)
        top = max(0, player_y - extent)
        right = min(backup_map.map_width, player_x + extent + 1)
        bottom = min(backup_map.map_height, player_y + extent + 1)

    rows: list[str] = []
    elevation_rows: list[list[int | None]] = []
    for y in range(top, bottom):
        row = []
        elevation_row: list[int | None] = []
        for x in range(left, right):
            index = ((y + MAP_OFFSET) * backup_map.width + x + MAP_OFFSET)
            block = struct.unpack_from("<H", ewram, grid_offset + index * 2)[0]
            metatile_id = block & 0x03FF
            collision = (block & MAPGRID_COLLISION_MASK) >> MAPGRID_COLLISION_SHIFT
            tile = "#" if metatile_id == MAPGRID_UNDEFINED or collision else "."
            elevation = None if metatile_id == MAPGRID_UNDEFINED else block >> MAPGRID_ELEVATION_SHIFT
            elevation_row.append(elevation)
            if (x, y) in warp_positions:
                tile = "D"
            if actor_by_xy.get((x, y)):
                tile = "N"
            if x == player_x and y == player_y:
                tile = "P"
            row.append(tile)
        rows.append("".join(row))
        elevation_rows.append(elevation_row)

    labels = actor_labels(map_data)
    actor_records = []
    for actor in actors:
        if actor.is_player:
            continue
        metadata = labels.get(actor.local_id, {})
        actor_records.append(
            {
                "local_id": actor.local_id,
                "label": metadata.get("label", f"object {actor.local_id}"),
                "graphics_id": metadata.get("graphics_id", f"graphics {actor.graphics_id}"),
                "x": actor.x,
                "y": actor.y,
                "facing": direction_name(actor.facing),
                "movement_direction": direction_name(actor.movement_direction),
                "elevation": actor.elevation,
                "moving": actor.moving,
            }
        )

    return {
        "profile": "Pokemon Emerald BPEE v1.0",
        "map": {
            "group": save.map_group,
            "number": save.map_num,
            "name": readable_name(str(map_data["name"])),
            "dimensions": {"width": backup_map.map_width, "height": backup_map.map_height},
        },
        "player": {
            "x": player_x,
            "y": player_y,
            "facing": next(
                (direction_name(actor.facing) for actor in actors if actor.is_player),
                "unknown",
            ),
            "movement": avatar,
        },
        "actors": actor_records,
        "grid": {
            "origin": {"x": left, "y": top},
            "legend": {"#": "blocked", ".": "walkable", "D": "warp", "N": "NPC/object", "P": "player"},
            "rows": rows,
            "elevation_rows": elevation_rows,
        },
    }


def render_text(observation: dict[str, object]) -> str:
    map_info = observation["map"]
    player = observation["player"]
    grid = observation["grid"]
    width = map_info["dimensions"]["width"]
    height = map_info["dimensions"]["height"]
    lines = [
        f"Map: {map_info['name']} ({width}x{height}; group {map_info['group']}, id {map_info['number']})",
        f"Player: ({player['x']}, {player['y']}), facing {player['facing']}",
        "Coordinates: x increases right; y increases down.",
    ]
    movement = player.get("movement")
    if movement:
        lines.append(
            "Movement: "
            f"running_state={movement['running_state']}, "
            f"tile_transition_state={movement['tile_transition_state']}"
        )
    lines.append(f"Grid origin: ({grid['origin']['x']}, {grid['origin']['y']})")
    lines.extend(grid["rows"])
    lines.append("Legend: # blocked, . walkable, D warp, N NPC/object, P player")
    lines.append("Elevation (hex; . is undefined):")
    lines.extend("".join("." if value is None else format(value, "X") for value in row) for row in grid["elevation_rows"])
    if observation["actors"]:
        lines.append("Actors:")
        for actor in observation["actors"]:
            motion = "moving" if actor["moving"] else "stationary"
            lines.append(
                f"  id {actor['local_id']} {actor['label']} ({actor['graphics_id']}): "
                f"({actor['x']}, {actor['y']}), facing {actor['facing']}, {motion}"
            )
    else:
        lines.append("Actors: live object array not found; map and player data are still available.")
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render a text map from Pokémon Emerald RAM")
    parser.add_argument("--host", default=DEFAULT_HOST, help="RetroArch NCI host")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="RetroArch NCI UDP port")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="NCI reply timeout")
    parser.add_argument("--radius", type=int, default=8, help="show this radius around the player; use --full-map for all tiles")
    parser.add_argument("--full-map", action="store_true", help="show the entire current map")
    parser.add_argument("--json", action="store_true", help="emit structured JSON instead of text")
    parser.add_argument("--rom-file", type=Path, help="optional ROM path; verifies BPEE game code and version 0")
    return parser.parse_args()


def verify_rom(path: Path | None, profile: dict[str, object]) -> None:
    if path is None:
        return
    with path.open("rb") as rom:
        rom.seek(0xAC)
        game_code = rom.read(4).decode("ascii", errors="replace")
        rom.seek(0xBC)
        version = rom.read(1)
    expected_code = str(profile["game_code"])
    expected_version = int(profile["version"])
    if game_code != expected_code or len(version) != 1 or version[0] != expected_version:
        raise ValueError(
            f"ROM profile mismatch: got {game_code} version "
            f"{version[0] if version else 'missing'}, expected {expected_code} version {expected_version}"
        )


def main() -> int:
    args = parse_args()
    try:
        if not (1 <= args.port <= 65535):
            raise ValueError("port must be between 1 and 65535")
        if args.timeout <= 0:
            raise ValueError("timeout must be positive")
        if args.radius < 1:
            raise ValueError("radius must be positive")
        profile = load_profile()
        verify_rom(args.rom_file, profile)
        client = NciClient(args.host, args.port, args.timeout)
        iwram, ewram = read_snapshot(client)
        pointers = pointer_slots(iwram)
        maps = profile_maps(profile)
        save = find_save_block(ewram, pointers, maps)
        map_data = maps[save.map_group][save.map_num]
        backup_map = find_backup_map(
            iwram,
            ewram,
            pointers,
            int(map_data["layout_width"]),
            int(map_data["layout_height"]),
        )
        actors, avatar = find_live_object_events(ewram, save, backup_map)
        observation = render_observation(
            profile,
            ewram,
            backup_map,
            save,
            actors,
            avatar,
            None if args.full_map else args.radius,
        )
        print(
            json.dumps(observation, ensure_ascii=False, indent=2)
            if args.json
            else render_text(observation)
        )
    except (OSError, RuntimeError, ValueError, struct.error) as exc:
        print(f"emerald-map: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
