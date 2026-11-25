#!/usr/bin/env python3
"""Small, read-only RetroArch NCI client for Emerald test automation.

The NCI listener should remain bound to localhost. This tool intentionally
does not expose memory writes or arbitrary command forwarding.
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import sys
import time
from dataclasses import dataclass
from pathlib import Path


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 55355
DEFAULT_TIMEOUT = 2.0
MAX_FRAME_ADVANCES = 4096
MAX_MEMORY_READ = 4096


@dataclass(frozen=True)
class NciClient:
    host: str
    port: int
    timeout: float

    def send_only(self, command: str) -> None:
        """Send a state/button command that does not require a reply."""
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.sendto(command.encode("utf-8"), (self.host, self.port))

    def send(self, command: str) -> str:
        """Send one NCI datagram and return the first reply."""
        payload = command.encode("utf-8")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(self.timeout)
            sock.sendto(payload, (self.host, self.port))
            try:
                response, _ = sock.recvfrom(65535)
            except TimeoutError as exc:
                raise RuntimeError(
                    f"No NCI response from {self.host}:{self.port} "
                    f"within {self.timeout:g}s"
                ) from exc
        return response.decode("utf-8", errors="replace").strip()


def emit(value: object) -> None:
    print(json.dumps(value, ensure_ascii=False, indent=2))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read-only RetroArch NCI helper for Pokémon Emerald tests"
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="NCI host (default: localhost)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="NCI UDP port")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="reply timeout in seconds")

    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="query RetroArch status")
    screenshot = commands.add_parser("screenshot", help="ask RetroArch to save a screenshot")
    screenshot.add_argument(
        "--directory",
        type=Path,
        help="wait for a new image in RetroArch's configured screenshot directory",
    )
    screenshot.add_argument("--wait-seconds", type=float, default=4.0)
    observe = commands.add_parser("observe", help="capture the running screen without querying status")
    observe.add_argument("--directory", type=Path, required=True)
    observe.add_argument("--wait-seconds", type=float, default=4.0)
    commands.add_parser("pause", help="pause RetroArch only if it is playing")
    commands.add_parser("resume", help="resume RetroArch only if it is paused")
    commands.add_parser("reset", help="reset the running content")
    frameadvance = commands.add_parser("frameadvance", help="advance frames while paused")
    frameadvance.add_argument("--count", type=int, default=1)
    frameadvance.add_argument("--interval-ms", type=int, default=20)
    read_memory = commands.add_parser("read-memory", help="read mapped core memory")
    read_memory.add_argument("address", help="hexadecimal system-memory address")
    read_memory.add_argument("length", type=int, help="number of bytes to read")
    return parser.parse_args()


def run(args: argparse.Namespace) -> object:
    if not (1 <= args.port <= 65535):
        raise ValueError("port must be between 1 and 65535")
    if args.timeout <= 0:
        raise ValueError("timeout must be positive")

    client = NciClient(args.host, args.port, args.timeout)
    if args.command == "status":
        return {"command": "GET_STATUS", "response": client.send("GET_STATUS")}
    if args.command == "screenshot":
        return capture_screenshot(client, args.directory, args.wait_seconds)
    if args.command == "observe":
        if args.wait_seconds <= 0:
            raise ValueError("wait-seconds must be positive")
        screenshot = capture_screenshot(client, args.directory, args.wait_seconds)
        return {
            "observed_at_unix": time.time(),
            "screenshot_path": screenshot["image_path"],
        }
    if args.command == "pause":
        status = client.send("GET_STATUS")
        if status.startswith("GET_STATUS PLAYING "):
            client.send_only("PAUSE_TOGGLE")
            action = "paused"
        elif status.startswith("GET_STATUS PAUSED "):
            action = "already-paused"
        else:
            raise RuntimeError(f"cannot determine pause state from NCI response: {status}")
        return {"command": "pause", "result": action, "before": status}
    if args.command == "resume":
        status = client.send("GET_STATUS")
        if status.startswith("GET_STATUS PAUSED "):
            client.send_only("PAUSE_TOGGLE")
            action = "resumed"
        elif status.startswith("GET_STATUS PLAYING "):
            action = "already-playing"
        else:
            raise RuntimeError(f"cannot determine pause state from NCI response: {status}")
        return {"command": "resume", "result": action, "before": status}
    if args.command == "reset":
        client.send_only("RESET")
        return {"command": "RESET", "sent": True}
    if args.command == "frameadvance":
        if not (1 <= args.count <= MAX_FRAME_ADVANCES):
            raise ValueError(f"count must be between 1 and {MAX_FRAME_ADVANCES}")
        if not (1 <= args.interval_ms <= 1000):
            raise ValueError("interval-ms must be between 1 and 1000")
        for index in range(args.count):
            client.send_only("FRAMEADVANCE")
            if index + 1 < args.count:
                time.sleep(args.interval_ms / 1000.0)
        return {
            "command": "FRAMEADVANCE",
            "count": args.count,
            "interval_ms": args.interval_ms,
            "sent": True,
        }
    if args.command == "read-memory":
        if not (1 <= args.length <= MAX_MEMORY_READ):
            raise ValueError(f"length must be between 1 and {MAX_MEMORY_READ}")
        if not re.fullmatch(r"(?:0[xX])?[0-9a-fA-F]+", args.address):
            raise ValueError("address must be hexadecimal, with an optional 0x prefix")
        address = int(args.address, 16)
        response = client.send(f"READ_CORE_MEMORY {address:X} {args.length}")
        values = re.search(r"^READ_CORE_MEMORY\s+\S+\s+(.+)$", response)
        if values and not values.group(1).lstrip().startswith("-1"):
            byte_tokens = values.group(1).split()
            try:
                data = [int(token, 16) for token in byte_tokens]
            except ValueError:
                data = None
        else:
            data = None
        return {
            "command": "READ_CORE_MEMORY",
            "address": f"0x{address:X}",
            "requested_length": args.length,
            "bytes": data,
            "response": response,
        }
    raise ValueError(f"unsupported command: {args.command}")


def image_files(directory: Path) -> dict[Path, tuple[int, int]]:
    extensions = {".png", ".jpg", ".jpeg", ".bmp"}
    return {
        path: (path.stat().st_mtime_ns, path.stat().st_size)
        for path in directory.iterdir()
        if path.is_file() and path.suffix.lower() in extensions
    }


def capture_screenshot(client: NciClient, directory: Path | None, wait_seconds: float) -> dict[str, object]:
    if wait_seconds <= 0:
        raise ValueError("wait-seconds must be positive")
    before = {}
    if directory:
        if not directory.is_dir():
            raise ValueError(f"screenshot directory does not exist: {directory}")
        before = image_files(directory)
    client.send_only("SCREENSHOT")
    result: dict[str, object] = {"command": "SCREENSHOT", "sent": True}
    if directory:
        deadline = time.monotonic() + wait_seconds
        while time.monotonic() < deadline:
            after = image_files(directory)
            changed = [
                path for path, signature in after.items()
                if before.get(path) != signature
            ]
            if changed:
                newest = max(changed, key=lambda path: path.stat().st_mtime_ns)
                result["image_path"] = str(newest.resolve())
                return result
            time.sleep(0.05)
        raise RuntimeError(
            f"RetroArch did not write a screenshot under {directory} "
            f"within {wait_seconds:g}s"
        )
    return result


def main() -> int:
    args = parse_args()
    try:
        emit(run(args))
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"emerald-agent-nci: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
