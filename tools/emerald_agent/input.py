#!/usr/bin/env python3
"""Send a short RetroArch-mapped game-button press on the local machine."""

from __future__ import annotations

import argparse
import ctypes
import os
import platform
import shutil
import subprocess
import sys
import time


MAX_PRESS_MS = 5000

# These are the mappings used by the isolated Emerald RetroArch configs:
# A=x, B=z, and the arrow keys control directions.
MAC_KEY_CODES = {
    "a": 7,       # X
    "b": 6,       # Z
    "up": 126,
    "down": 125,
    "left": 123,
    "right": 124,
    "start": 36,  # Return
    "select": 60, # Right Shift
}

LINUX_KEY_CODES = {
    "a": 45,      # KEY_X
    "b": 44,      # KEY_Z
    "up": 103,    # KEY_UP
    "down": 108,  # KEY_DOWN
    "left": 105,  # KEY_LEFT
    "right": 106, # KEY_RIGHT
    "start": 28,  # KEY_ENTER
    "select": 54, # KEY_RIGHTSHIFT
}


def mac_key_event(key_code: int, pressed: bool) -> None:
    core_graphics = ctypes.CDLL(
        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"
    )
    core_foundation = ctypes.CDLL(
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
    )
    create_event = core_graphics.CGEventCreateKeyboardEvent
    create_event.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_bool]
    create_event.restype = ctypes.c_void_p
    post_event = core_graphics.CGEventPost
    post_event.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
    post_event.restype = None
    release = core_foundation.CFRelease
    release.argtypes = [ctypes.c_void_p]
    release.restype = None

    event = create_event(None, key_code, pressed)
    if not event:
        raise RuntimeError(f"could not create macOS keyboard event for key code {key_code}")
    try:
        # kCGHIDEventTap = 0
        post_event(0, event)
    finally:
        release(event)


def linux_key_event(key_code: int, pressed: bool, socket_path: str | None = None) -> None:
    executable = shutil.which("ydotool")
    if not executable:
        raise RuntimeError("ydotool is required on Linux")
    event = f"{key_code}:{1 if pressed else 0}"
    environment = os.environ.copy()
    if socket_path:
        environment["YDOTOOL_SOCKET"] = socket_path
    result = subprocess.run(
        [executable, "key", event],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )
    if result.returncode:
        message = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"ydotool failed ({result.returncode}): {message}")


def press(button: str, duration_ms: int, ydotool_socket: str | None = None) -> None:
    if not (1 <= duration_ms <= MAX_PRESS_MS):
        raise ValueError(f"duration must be between 1 and {MAX_PRESS_MS} ms")

    system = platform.system()
    if system == "Darwin":
        mapping = MAC_KEY_CODES
        send = mac_key_event
    elif system == "Linux":
        mapping = LINUX_KEY_CODES
        send = lambda code, pressed: linux_key_event(code, pressed, ydotool_socket)
    else:
        raise RuntimeError(f"unsupported input platform: {system}")

    if button not in mapping:
        raise ValueError(f"unsupported button: {button}")

    key_code = mapping[button]
    send(key_code, True)
    try:
        time.sleep(duration_ms / 1000.0)
    finally:
        send(key_code, False)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Send one short button press to the focused RetroArch instance"
    )
    parser.add_argument(
        "button",
        choices=("a", "b", "up", "down", "left", "right", "start", "select"),
    )
    parser.add_argument("--duration-ms", type=int, default=120)
    parser.add_argument("--ydotool-socket", help="custom Linux ydotoold socket path")
    args = parser.parse_args()
    try:
        press(args.button, args.duration_ms, args.ydotool_socket)
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"emerald-agent-input: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
