#!/usr/bin/env python3
"""Capture selected GBMicrotest PPU latch probes without pixel pass/fail gates."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


PROBES = {
    "800-ppu-latch-scx": {
        "sha256": "8ea89aca724cef5cf05bba2e633daf0d58bd8a7c1d6da764e3e8d7cd844b3263",
        "note": "SCX write with delay 8; source comments place the scroll transition between delays 7 and 8.",
    },
    "801-ppu-latch-scy": {
        "sha256": "8cee8c41767eaefdf299bd84931f3c91c079fd9580f8a6f76aa470280678d98a",
        "note": "SCY write with delay 9; source comments describe two scrolled columns.",
    },
    "802-ppu-latch-tileselect": {
        "sha256": "f8bee200e25793d56228cc362593bf8a46877b1ae66b0393f531e9b1e7dd8b4b",
        "note": "LCDC tile-data select changes during the scanline with delay 15.",
    },
    "803-ppu-latch-bgdisplay": {
        "sha256": "146c99504449ad88ca25ce43b9856b17d93283b626d7e4d577e9a6c5fa56241a",
        "note": "LCDC background enable changes during the scanline with delay 9.",
    },
}
FRAME_COUNT = 4
SUMMARY = re.compile(
    r"frames_requested=(\d+) video_frames=(\d+) audio_frames=(\d+) "
    r"save_ram_bytes=(\d+) width=(\d+) height=(\d+)\s*"
)
FRAME_BYTES = 160 * 144 * 3


def write_contact_sheet(output: Path, names) -> None:
    width, height = 640, 576
    sheet = bytearray(width * height * 3)
    for index, name in enumerate(names):
        source = (output / f"{name}.rgb").read_bytes()
        if len(source) != FRAME_BYTES:
            raise ValueError(f"cannot compose invalid frame for {name}")
        origin_x, origin_y = (index % 2) * 160, (index // 2) * 288
        for y in range(144):
            row = source[y * 160 * 3:(y + 1) * 160 * 3]
            expanded = b"".join(row[x:x + 3] * 2 for x in range(0, len(row), 3))
            for repeat_y in range(2):
                start = ((origin_y + y * 2 + repeat_y) * width + origin_x * 2) * 3
                sheet[start:start + len(expanded)] = expanded
    header = (
        "P6\n# TL=800 SCX, TR=801 SCY, BL=802 LCDC.4, BR=803 LCDC.0\n"
        f"{width} {height}\n255\n"
    ).encode()
    (output / "contact-sheet.ppm").write_bytes(header + sheet)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-dir", required=True, type=Path,
                        help="GBMicrotest checkout or extracted suite containing bin/")
    parser.add_argument("--core", required=True, type=Path)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path,
                        default=Path("build/test-roms/gbmicrotest-ppu-latch"))
    args = parser.parse_args()

    suite = args.suite_dir.resolve()
    rom_dir = suite / "bin"
    core = args.core.resolve()
    runner = args.runner.resolve()
    output = args.output_dir.resolve()
    if not core.is_file() or not runner.is_file():
        parser.error(f"core or ROM runner is missing: {core} ; {runner}")
    if output == suite or output in suite.parents or suite in output.parents:
        parser.error("output directory must stay separate from the GBMicrotest suite")

    roms = {}
    for name, profile in PROBES.items():
        path = rom_dir / f"{name}.gb"
        if not path.is_file():
            parser.error(f"missing GBMicrotest ROM: {path}")
        digest = sha256(path.read_bytes())
        if digest != profile["sha256"]:
            parser.error(f"unexpected checksum for {path}: {digest}")
        roms[name] = path

    output.mkdir(parents=True, exist_ok=True)
    (output / "report.json").unlink(missing_ok=True)
    (output / "contact-sheet.ppm").unlink(missing_ok=True)
    report = {
        "suite": "aappleby/gbmicrotest",
        "suite_license": "MIT",
        "author_reported_hardware": "DMG-CPU-08; exact revision uncertain",
        "core_sha256": sha256(core.read_bytes()),
        "runner_sha256": sha256(runner.read_bytes()),
        "frames": FRAME_COUNT,
        "scope": (
            "Diagnostic captures only. Review the output alongside upstream source comments. "
            "No frame is treated as a pixel-accurate hardware oracle or automatic pass/fail gate."
        ),
        "probes": {},
    }
    failed = False
    for name, path in roms.items():
        artifacts = {suffix: output / f"{name}.{suffix}"
                     for suffix in ("rgb", "ppm", "srm", "log")}
        for artifact in artifacts.values():
            artifact.unlink(missing_ok=True)
        command = [str(runner), str(core), str(path), str(FRAME_COUNT),
                   str(artifacts["rgb"]), str(artifacts["srm"])]
        run = subprocess.run(command, capture_output=True, text=True,
                             timeout=60, check=False)
        artifacts["log"].write_text(run.stdout + run.stderr, encoding="utf-8")
        entry = {
            "rom_sha256": PROBES[name]["sha256"],
            "upstream_note": PROBES[name]["note"],
            "command": command,
            "runner_exit_code": run.returncode,
            "runner_output": run.stdout.strip(),
        }
        match = SUMMARY.fullmatch(run.stdout)
        if run.returncode != 0 or not match:
            entry["capture_error"] = "runner failed or returned a malformed summary"
            failed = True
        else:
            requested, video, _audio, save_size, width, height = map(int, match.groups())
            rgb = artifacts["rgb"].read_bytes() if artifacts["rgb"].is_file() else b""
            save = artifacts["srm"].read_bytes() if artifacts["srm"].is_file() else b""
            if (requested != FRAME_COUNT or video != FRAME_COUNT or
                    (width, height) != (160, 144) or len(rgb) != width * height * 3 or
                    len(save) != save_size):
                entry["capture_error"] = "runner output is incomplete or inconsistent"
                failed = True
            else:
                artifacts["ppm"].write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)
                entry.update(
                    capture_error=None,
                    frame_sha256=sha256(rgb),
                    sram_bytes=save_size,
                    image=str(artifacts["ppm"].relative_to(output)),
                )
        report["probes"][name] = entry

    report["capture_status"] = "failed" if failed else "captured"
    if not failed:
        try:
            write_contact_sheet(output, list(PROBES))
            report["contact_sheet"] = "contact-sheet.ppm"
            report["contact_sheet_order"] = list(PROBES)
        except (OSError, ValueError) as error:
            report["capture_status"] = "failed"
            report["contact_sheet_error"] = str(error)
            failed = True
    report_path = output / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Report: {report_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
