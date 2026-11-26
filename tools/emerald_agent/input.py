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

WINDOWS_KEY_CODES = {
    "a": 0x58, "b": 0x5A,  # X, Z
    "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27,
    "start": 0x0D, "select": 0xA1,  # Return, right Shift
}


def windows_key_event(key_code: int, pressed: bool) -> None:
    """Inject a physical key into the focused window, checking delivery."""
    from ctypes import wintypes

    class KeyboardInput(ctypes.Structure):
        _fields_ = [("vk", wintypes.WORD), ("scan", wintypes.WORD),
                    ("flags", wintypes.DWORD), ("time", wintypes.DWORD),
                    ("extra", ctypes.c_size_t)]

    class MouseInput(ctypes.Structure):
        _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG),
                    ("data", wintypes.DWORD), ("flags", wintypes.DWORD),
                    ("time", wintypes.DWORD), ("extra", ctypes.c_size_t)]

    class HardwareInput(ctypes.Structure):
        _fields_ = [("message", wintypes.DWORD), ("low", wintypes.WORD),
                    ("high", wintypes.WORD)]

    class Payload(ctypes.Union):
        _fields_ = [("keyboard", KeyboardInput), ("mouse", MouseInput),
                    ("hardware", HardwareInput)]

    class Input(ctypes.Structure):
        _fields_ = [("type", wintypes.DWORD), ("payload", Payload)]

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.MapVirtualKeyW.argtypes = [wintypes.UINT, wintypes.UINT]
    user32.MapVirtualKeyW.restype = wintypes.UINT
    user32.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(Input), ctypes.c_int]
    user32.SendInput.restype = wintypes.UINT
    scan = user32.MapVirtualKeyW(key_code, 0)
    if not scan:
        raise ctypes.WinError(ctypes.get_last_error())
    flags = 0x0008 | (0 if pressed else 0x0002)  # SCANCODE, KEYUP
    if key_code in (0x25, 0x26, 0x27, 0x28):
        flags |= 0x0001  # extended arrow keys
    event = Input(type=1, payload=Payload(keyboard=KeyboardInput(0, scan, flags, 0, 0)))
    if user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(Input)) != 1:
        raise ctypes.WinError(ctypes.get_last_error())


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
    elif system == "Windows":
        mapping = WINDOWS_KEY_CODES
        send = windows_key_event
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
