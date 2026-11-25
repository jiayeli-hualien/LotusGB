#!/usr/bin/env python3
"""One-step observe/act interface for a locally running RetroArch instance."""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

if __package__:
    from .input import press
    from .nci import DEFAULT_HOST, DEFAULT_PORT, DEFAULT_TIMEOUT, NciClient, capture_screenshot
else:
    from input import press
    from nci import DEFAULT_HOST, DEFAULT_PORT, DEFAULT_TIMEOUT, NciClient, capture_screenshot


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Observe or take one game action, then capture the result"
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--ydotool-socket", help="custom Linux ydotoold socket path")
    parser.add_argument("--player", choices=("host", "client"), required=True)
    parser.add_argument("--screenshot-dir", type=Path, required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    observe = commands.add_parser("observe", help="capture current state without sending game input")
    observe.add_argument("--wait-seconds", type=float, default=4.0)
    act = commands.add_parser("act", help="send one button press and capture the result")
    act.add_argument(
        "button",
        choices=("a", "b", "up", "down", "left", "right", "start", "select"),
    )
    act.add_argument("--duration-ms", type=int, default=120)
    act.add_argument("--settle-ms", type=int, default=100)
    act.add_argument("--wait-seconds", type=float, default=4.0)
    args = parser.parse_args()

    try:
        client = NciClient(args.host, args.port, args.timeout)
        action = None
        if args.command == "act":
            if not (0 <= args.settle_ms <= 5000):
                raise ValueError("settle-ms must be between 0 and 5000")
            press(args.button, args.duration_ms, args.ydotool_socket)
            action = {"button": args.button, "duration_ms": args.duration_ms}
            time.sleep(args.settle_ms / 1000.0)

        screenshot = capture_screenshot(client, args.screenshot_dir, args.wait_seconds)
        print(json.dumps({
            "player": args.player,
            "observed_at_unix": time.time(),
            "action": action,
            "screenshot_path": screenshot["image_path"],
        }, ensure_ascii=False, indent=2))
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"emerald-agent: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
