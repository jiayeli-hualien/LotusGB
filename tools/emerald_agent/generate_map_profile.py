#!/usr/bin/env python3
"""Build the BPEE Emerald map profile from a pret/pokeemerald checkout."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def build_profile(source: Path) -> dict[str, object]:
    maps_dir = source / "data" / "maps"
    groups_file = maps_dir / "map_groups.json"
    layouts_file = source / "data" / "layouts" / "layouts.json"
    groups_data = json.loads(groups_file.read_text(encoding="utf-8"))
    layouts_data = json.loads(layouts_file.read_text(encoding="utf-8"))["layouts"]
    layouts = {
        layout["id"]: {
            "layout_id": index + 1,
            "width": layout["width"],
            "height": layout["height"],
        }
        for index, layout in enumerate(layouts_data)
    }

    groups = []
    for group_symbol in groups_data["group_order"]:
        maps = []
        for map_name in groups_data[group_symbol]:
            map_path = maps_dir / map_name / "map.json"
            map_data = json.loads(map_path.read_text(encoding="utf-8"))
            layout_id = map_data["layout"]
            if layout_id not in layouts:
                raise ValueError(f"unknown layout {layout_id!r} in {map_path}")
            layout = layouts[layout_id]
            actors = []
            for index, actor in enumerate(map_data.get("object_events", []), start=1):
                actors.append(
                    {
                        "local_id_num": index,
                        "local_id": actor.get("local_id"),
                        "graphics_id": actor.get("graphics_id", "OBJ_EVENT"),
                        "script": actor.get("script"),
                    }
                )
            warps = [
                {
                    "x": warp["x"],
                    "y": warp["y"],
                    "dest_map": warp.get("dest_map"),
                    "dest_warp_id": warp.get("dest_warp_id"),
                }
                for warp in map_data.get("warp_events", [])
            ]
            maps.append(
                {
                    "name": map_name,
                    "map_id": map_data["id"],
                    "layout": layout_id,
                    "layout_id": layout["layout_id"],
                    "layout_width": layout["width"],
                    "layout_height": layout["height"],
                    "actors": actors,
                    "warps": warps,
                }
            )
        groups.append({"symbol": group_symbol, "maps": maps})

    return {
        "game_code": "BPEE",
        "version": 0,
        "source": "pret/pokeemerald data/maps and data/layouts",
        "groups": groups,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="path to a pret/pokeemerald checkout")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).with_name("emerald_bpee_v1_map_data.json"),
    )
    args = parser.parse_args()
    profile = build_profile(args.source)
    args.output.write_text(
        json.dumps(profile, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Wrote {sum(len(group['maps']) for group in profile['groups'])} maps to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
