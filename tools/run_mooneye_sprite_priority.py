#!/usr/bin/env python3
"""Compare LotusGB's DMG output with Mooneye's manual sprite-priority image."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


SIZE = (160, 144)
TEST_RELATIVE_PATH = Path("manual-only/sprite_priority.gb")
EXPECTED_RELATIVE_PATH = Path("manual-only/sprite_priority-dmg.png")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--suite-dir", required=True, type=Path,
                        help="Mooneye test suite directory from game-boy-test-roms v7.0")
    parser.add_argument("--output-dir", type=Path,
                        default=Path("build/test-roms/mooneye-sprite-priority"))
    args = parser.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("This optional visual check requires Pillow; see docs/ppu-scrolling.md.",
              file=sys.stderr)
        return 2

    suite = args.suite_dir.resolve()
    runner = args.runner.resolve()
    rom_path = suite / TEST_RELATIVE_PATH
    expected_path = suite / EXPECTED_RELATIVE_PATH
    output = args.output_dir.resolve()
    if not runner.is_file():
        print(f"Missing PPU frame runner: {runner}", file=sys.stderr)
        return 2
    if not rom_path.is_file() or not expected_path.is_file():
        print("Missing Mooneye sprite-priority ROM or DMG reference image under "
              f"{suite}", file=sys.stderr)
        return 2
    if output == suite or suite in output.parents:
        print("Output directory must stay outside the reference suite", file=sys.stderr)
        return 2

    output.mkdir(parents=True, exist_ok=True)
    shades_path = output / "sprite_priority.shades"
    actual_path = output / "sprite_priority.png"
    diff_path = output / "sprite_priority.diff.png"
    log_path = output / "runner.log"
    report_path = output / "report.json"
    for artifact in (shades_path, actual_path, diff_path, log_path, report_path):
        artifact.unlink(missing_ok=True)

    rom = rom_path.read_bytes()
    expected_bytes = expected_path.read_bytes()
    report = {
        "test": "manual-only/sprite_priority (DMG)",
        "passed": False,
        "rom_sha256": sha256(rom),
        "expected_sha256": sha256(expected_bytes),
        "runner_sha256": sha256(runner.read_bytes()),
        "scope": "DMG sprite priority screenshot; does not validate exact OBJ fetch timing.",
    }

    try:
        run = subprocess.run([str(runner), str(rom_path), str(shades_path)],
                             capture_output=True, text=True, timeout=60, check=False)
        log_path.write_text(run.stdout + run.stderr, encoding="utf-8")
        report["runner_exit_code"] = run.returncode
        if run.returncode != 0:
            report["error"] = "Frame capture failed; inspect runner.log."
        else:
            shades = shades_path.read_bytes()
            if len(shades) != SIZE[0] * SIZE[1] or any(value > 3 for value in shades):
                raise ValueError("invalid captured shade framebuffer")
            actual = Image.frombytes(
                "L", SIZE, bytes(255 - shade * 85 for shade in shades)
            ).convert("RGB")
            with Image.open(expected_path) as source:
                expected = source.convert("RGB")
            if expected.size != SIZE:
                raise ValueError(f"unexpected reference dimensions: {expected.size}")

            actual_data = actual.tobytes()
            expected_data = expected.tobytes()
            different = [
                actual_data[offset:offset + 3] != expected_data[offset:offset + 3]
                for offset in range(0, len(actual_data), 3)
            ]
            actual.save(actual_path)
            diff = Image.new("RGB", SIZE)
            diff.putdata([(255, 0, 0) if value else (0, 0, 0) for value in different])
            diff.save(diff_path)
            report.update(
                shade_frame_sha256=sha256(shades),
                actual_frame_sha256=sha256(actual_data),
                different_pixels=sum(different),
                passed=not any(different),
            )
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)

    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if report["passed"]:
        print("PASS manual-only/sprite_priority (DMG): 0 / 23040 differing pixels")
        print(f"Report: {report_path}")
        return 0
    print("FAIL manual-only/sprite_priority (DMG): "
          f"{report.get('different_pixels', report.get('error'))}")
    print(f"Report: {report_path}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
