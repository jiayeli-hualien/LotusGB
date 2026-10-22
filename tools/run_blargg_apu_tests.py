#!/usr/bin/env python3
"""Run selected external Blargg DMG APU ROMs against the built libretro core."""

from __future__ import annotations

import argparse
import hashlib
import shlex
import struct
import subprocess
import sys
import zlib
from datetime import datetime
from pathlib import Path


WIDTH = 160
HEIGHT = 144
CARTRIDGE_TYPE_OFFSET = 0x0147
MBC1_RAM_BATTERY_CARTRIDGE = 0x03

CASES = (
    ("09", "wave read while on", "378b86f6a25daa16855260d7ef0c24e48146ba05df434a0b5413983a1447e875"),
    ("10", "wave trigger while on", "fa63c8ed7473411e54285d318e33bf23ff6d637ed2caa7555ddeaf80578e3279"),
    ("12", "wave write while on", "2efbecd2c6d40928d44f45da4f634626bcd2790165b3949aedfe377c73913774"),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def get_test_result(save_ram: bytes) -> str:
    passed = save_ram.rfind(b"\nPassed\n")
    failed = save_ram.rfind(b"\nFailed\n")
    if passed < 0 and failed < 0:
        return "NO RESULT"
    return "Passed" if passed > failed else "Failed"


def find_rom(rom_dir: Path, test_id: str, title: str) -> Path | None:
    path = rom_dir / f"{test_id}-{title}.gb"
    return path if path.is_file() else None


def png_chunk(kind: bytes, data: bytes) -> bytes:
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))


def write_png(rgb: bytes, path: Path) -> None:
    expected_size = WIDTH * HEIGHT * 3
    if len(rgb) != expected_size:
        raise ValueError(f"runner returned {len(rgb)} RGB bytes; expected {expected_size}")

    scanlines = b"".join(
        b"\x00" + rgb[row * WIDTH * 3 : (row + 1) * WIDTH * 3]
        for row in range(HEIGHT)
    )
    png = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">2I5B", WIDTH, HEIGHT, 8, 2, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(scanlines, level=9))
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def unique_run_dir(root: Path) -> Path:
    stamp = datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")
    candidate = root / stamp
    suffix = 1
    while candidate.exists():
        candidate = root / f"{stamp}-{suffix}"
        suffix += 1
    candidate.mkdir(parents=True)
    return candidate


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=Path, help="built LotusGB libretro .so")
    parser.add_argument("--runner", required=True, type=Path, help="headless libretro runner")
    parser.add_argument("--rom-dir", required=True, type=Path, help="external dmg_sound ROM directory")
    parser.add_argument("--frames", required=True, type=int, help="emulated frames per ROM")
    parser.add_argument("--output-root", required=True, type=Path, help="directory for run records")
    args = parser.parse_args()

    if args.frames <= 0:
        parser.error("--frames must be positive")
    if not args.core.is_file():
        parser.error(f"libretro core not found: {args.core}")
    if not args.runner.is_file():
        parser.error(f"ROM runner not found: {args.runner}")
    if not args.rom_dir.is_dir():
        parser.error(
            f"ROM directory not found: {args.rom_dir}\n"
            "Set TEST_ROM_DIR to the directory containing the external Blargg ROMs."
        )

    run_dir = unique_run_dir(args.output_root)
    rom_output_dir = run_dir / "roms"
    screenshot_dir = run_dir / "screenshots"
    log_dir = run_dir / "logs"
    save_ram_dir = run_dir / "save_ram"
    rom_output_dir.mkdir()
    screenshot_dir.mkdir()
    log_dir.mkdir()
    save_ram_dir.mkdir()

    rows: list[dict[str, str]] = []
    executed = 0
    failed = 0

    for test_id, title, expected_sha256 in CASES:
        source = find_rom(args.rom_dir, test_id, title)
        if source is None:
            rows.append(
                {
                    "test": f"{test_id} {title}",
                    "status": "SKIPPED",
                    "source": "ROM file not present",
                    "source_sha256": "—",
                    "cartridge_type": "—",
                    "save_ram": "—",
                    "screenshot": "—",
                    "log": "—",
                    "manual_result": "—",
                    "detail": f"Expected original {test_id}-{title}.gb",
                }
            )
            continue

        source_digest = sha256(source)
        try:
            data = source.read_bytes()
            if len(data) != 0x8000:
                raise ValueError(f"ROM size is {len(data)} bytes; expected 32768")
            cartridge_type = data[CARTRIDGE_TYPE_OFFSET]
            if cartridge_type != MBC1_RAM_BATTERY_CARTRIDGE:
                raise ValueError(
                    f"unexpected cartridge type ${cartridge_type:02X}; expected $03"
                )
            if source_digest != expected_sha256:
                raise ValueError(
                    f"source ROM SHA-256 is {source_digest}; expected {expected_sha256}"
                )
        except (OSError, ValueError) as error:
            failed += 1
            rows.append(
                {
                    "test": f"{test_id} {title}",
                    "status": "FAILED checksum/input validation",
                    "source": str(source),
                    "source_sha256": source_digest,
                    "cartridge_type": "—",
                    "save_ram": "—",
                    "screenshot": "—",
                    "log": "—",
                    "manual_result": "—",
                    "detail": str(error),
                }
            )
            continue

        rom_path = rom_output_dir / f"{test_id}.gb"
        raw_frame_path = run_dir / f"{test_id}.rgb"
        screenshot_path = screenshot_dir / f"{test_id}.png"
        save_ram_path = save_ram_dir / f"{test_id}.srm"
        log_path = log_dir / f"{test_id}.log"
        rom_path.write_bytes(data)
        command = [
            str(args.runner.resolve()),
            str(args.core.resolve()),
            str(rom_path.resolve()),
            str(args.frames),
            str(raw_frame_path.resolve()),
            str(save_ram_path.resolve()),
        ]

        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        log_path.write_text(
            f"$ {shlex.join(command)}\n"
            f"exit_code: {completed.returncode}\n\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}",
            encoding="utf-8",
        )
        if completed.returncode != 0:
            failed += 1
            rows.append(
                {
                    "test": f"{test_id} {title}",
                    "status": f"FAILED runner exit {completed.returncode}",
                    "source": str(source),
                    "source_sha256": source_digest,
                    "cartridge_type": f"`${cartridge_type:02X}`",
                    "save_ram": "—",
                    "screenshot": "—",
                    "log": f"[runner log]({log_path.relative_to(run_dir)})",
                    "manual_result": "—",
                    "detail": completed.stderr.strip().replace("|", "\\|") or "See log",
                }
            )
            continue

        try:
            write_png(raw_frame_path.read_bytes(), screenshot_path)
            save_ram = save_ram_path.read_bytes()
        except (OSError, ValueError) as error:
            failed += 1
            rows.append(
                {
                    "test": f"{test_id} {title}",
                    "status": "FAILED screenshot capture",
                    "source": str(source),
                    "source_sha256": source_digest,
                    "cartridge_type": f"`${cartridge_type:02X}`",
                    "save_ram": f"[{save_ram_path.stat().st_size} bytes]({save_ram_path.relative_to(run_dir)})",
                    "screenshot": "—",
                    "log": f"[runner log]({log_path.relative_to(run_dir)})",
                    "manual_result": "—",
                    "detail": str(error),
                }
            )
            continue

        raw_frame_path.unlink()
        result = get_test_result(save_ram)
        if result != "Passed":
            failed += 1
        executed += 1
        rows.append(
            {
                "test": f"{test_id} {title}",
                "status": "PASSED" if result == "Passed" else result,
                "source": str(source),
                "source_sha256": source_digest,
                "cartridge_type": f"`${cartridge_type:02X}`",
                "save_ram": f"[{save_ram_path.stat().st_size} bytes]({save_ram_path.relative_to(run_dir)})",
                "screenshot": f"[PNG]({screenshot_path.relative_to(run_dir)})",
                "log": f"[runner log]({log_path.relative_to(run_dir)})",
                "manual_result": result,
                "detail": completed.stdout.strip().replace("|", "\\|") or "—",
            }
        )

    core_digest = sha256(args.core)
    report = [
        "# Blargg DMG APU ROM run",
        "",
        f"- Time: {datetime.now().astimezone().isoformat(timespec='seconds')}",
        f"- Core: `{args.core}` (SHA-256 `{core_digest}`)",
        f"- ROM directory: `{args.rom_dir}`",
        f"- Frames per ROM: {args.frames}",
        "- Frontend: headless harness calling the built libretro shared core",
        "- Startup: LotusGB DMG post-boot profile; original ROM header and bytes are preserved.",
        "- Mapper: MBC1 with 8 KiB battery-backed external RAM, exposed as libretro save RAM.",
        "- Result check: review the on-screen result and the captured save RAM dump.",
        "",
        "| Test | Run status | Source ROM SHA-256 | Cart type | Screenshot | Save RAM | Log | Manual result | Details |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for row in rows:
        values = [row[key].replace("|", "\\|").replace("\n", " ") for key in (
            "test", "status", "source_sha256", "cartridge_type", "screenshot",
            "save_ram", "log", "manual_result", "detail"
        )]
        report.append("| " + " | ".join(values) + " |")
    report.extend(
        [
            "",
            f"Summary: {executed} ROM(s) captured, {failed} failure(s), "
            f"{sum(row['status'] == 'SKIPPED' for row in rows)} skipped.",
            "",
            "The runner reads the final `Passed` or `Failed` line from the captured external RAM; screenshots are retained for visual inspection.",
            "",
        ]
    )
    report_path = run_dir / "report.md"
    report_path.write_text("\n".join(report), encoding="utf-8")

    print(f"Captured {executed} ROM(s); {failed} failure(s).")
    print(f"Run record: {report_path}")
    if executed == 0:
        print("No ROMs ran. Add Blargg ROMs to the external directory shown above.", file=sys.stderr)
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
