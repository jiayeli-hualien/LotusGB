#!/usr/bin/env python3
"""Run selected Mooneye DMG PPU/interrupt or timer checks and save a local report."""

import argparse
from pathlib import Path
import subprocess
import sys
import time


TESTS = (
    "acceptance/ppu/intr_1_2_timing-GS.gb",
    "acceptance/ppu/intr_2_0_timing.gb",
    "acceptance/ppu/intr_2_mode3_timing.gb",
    "acceptance/ppu/intr_2_mode0_timing.gb",
    "acceptance/ppu/intr_2_oam_ok_timing.gb",
    "acceptance/intr_timing.gb",
    "acceptance/halt_ime1_timing.gb",
    "acceptance/ppu/stat_lyc_onoff.gb",
    "acceptance/ppu/vblank_stat_intr-GS.gb",
    "acceptance/ppu/lcdon_write_timing-GS.gb",
    "acceptance/ppu/lcdon_timing-GS.gb",
)


TIMER_TESTS = tuple(f"acceptance/timer/{name}.gb" for name in (
    "div_write", "rapid_toggle", "tim00", "tim00_div_trigger",
    "tim01", "tim01_div_trigger", "tim10", "tim10_div_trigger",
    "tim11", "tim11_div_trigger", "tima_reload",
    "tima_write_reloading", "tma_write_reloading",
))


# Diagnostic coverage: known failures are retained to track unimplemented edges.
EXTENDED_TESTS = (
    "acceptance/ppu/intr_2_mode0_timing_sprites.gb",
    "acceptance/ppu/hblank_ly_scx_timing-GS.gb",
    "acceptance/ppu/stat_irq_blocking.gb",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--suite-dir", required=True, type=Path,
                        help="Root of an extracted Mooneye test-suite archive")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--group", choices=("ppu", "timer", "all"), default="ppu",
                        help="Selected regression group (default: ppu)")
    parser.add_argument("--extended", action="store_true",
                        help="Include diagnostic PPU cases with known failures")
    args = parser.parse_args()
    if args.extended and args.group == "timer":
        parser.error("--extended adds PPU diagnostics; use --group ppu or all")
    tests = () if args.group == "timer" else TESTS
    if args.extended:
        tests += EXTENDED_TESTS
    if args.group in ("timer", "all"):
        tests += TIMER_TESTS

    missing = [name for name in tests if not (args.suite_dir / name).is_file()]
    if missing:
        print("Missing selected Mooneye ROMs:", file=sys.stderr)
        for name in missing:
            print(f"  {args.suite_dir / name}", file=sys.stderr)
        return 2

    args.output_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    failed = False
    for name in tests:
        rom_path = args.suite_dir / name
        log_path = args.output_dir / f"{rom_path.stem}.log"
        started = time.monotonic()
        result = subprocess.run(
            [str(args.runner), str(rom_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        elapsed = time.monotonic() - started
        output = result.stdout.rstrip()
        log_path.write_text(output + "\n", encoding="utf-8")
        status = "PASS" if result.returncode == 0 else "FAIL"
        failed |= result.returncode != 0
        rows.append((name, status, elapsed, log_path.name, output))
        print(f"{status:4} {name} ({elapsed:.2f}s)")
        if output:
            print(f"     {output.splitlines()[-1]}")

    report = [
        f"# Mooneye timing checks: {args.group}",
        "",
        "Run with the local LotusGB system runner and externally supplied ROMs.",
        "The ROM archive is not copied into this repository.",
        "",
        "| Test | Result | Runtime | Log |",
        "| --- | --- | ---: | --- |",
    ]
    report.extend(
        f"| `{name}` | **{status}** | {elapsed:.2f}s | [`{log}`]({log}) |"
        for name, status, elapsed, log, _ in rows
    )
    report.append("")
    (args.output_dir / "report.md").write_text("\n".join(report), encoding="utf-8")
    print(f"Report: {args.output_dir / 'report.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
