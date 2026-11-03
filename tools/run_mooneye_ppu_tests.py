#!/usr/bin/env python3
"""Run selected Mooneye DMG or CGB CPU, PPU, interrupt, timer, serial, DMA, or mapper checks."""

import argparse
import hashlib
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
    "acceptance/ppu/intr_2_mode0_timing_sprites.gb",
    "acceptance/ppu/hblank_ly_scx_timing-GS.gb",
    "acceptance/ppu/stat_irq_blocking.gb",
)


TIMER_TESTS = tuple(f"acceptance/timer/{name}.gb" for name in (
    "div_write", "rapid_toggle", "tim00", "tim00_div_trigger",
    "tim01", "tim01_div_trigger", "tim10", "tim10_div_trigger",
    "tim11", "tim11_div_trigger", "tima_reload",
    "tima_write_reloading", "tma_write_reloading",
)) + ("acceptance/div_timing.gb",)


CPU_TESTS = tuple(f"acceptance/{name}.gb" for name in (
    "add_sp_e_timing", "call_cc_timing", "call_cc_timing2",
    "call_timing", "call_timing2", "jp_cc_timing", "jp_timing",
    "ld_hl_sp_e_timing", "pop_timing", "push_timing", "ret_cc_timing",
    "ret_timing", "rst_timing",
)) + tuple(f"acceptance/{group}/{name}.gb" for group, names in (
    ("bits", ("mem_oam", "reg_f", "unused_hwio-GS")),
    ("instr", ("daa",)),
) for name in names)


DMA_TESTS = tuple(f"acceptance/oam_dma/{name}.gb" for name in (
    "basic", "reg_read", "sources-GS",
)) + tuple(f"acceptance/{name}.gb" for name in (
    "oam_dma_start", "oam_dma_timing", "oam_dma_restart",
))


INTERRUPT_TESTS = tuple(f"acceptance/{name}.gb" for name in (
    "ei_sequence", "ei_timing", "rapid_di_ei", "di_timing-GS",
    "reti_intr_timing", "reti_timing", "halt_ime0_ei",
    "halt_ime0_nointr_timing", "halt_ime1_timing2-GS", "if_ie_registers",
)) + ("acceptance/interrupts/ie_push.gb",)


SERIAL_TESTS = ("acceptance/serial/boot_sclk_align-dmgABCmgb.gb",)

# The PPU check is independent of the boot ROM and can run with CGB-E's
# synthetic startup. The remaining probes inspect state established by the
# actual CGB boot program, so expose them as a separate BIOS-dependent group.
CGB_TESTS = ("misc/ppu/vblank_stat_intr-C.gb",)
CGB_BOOT_TESTS = tuple(f"misc/{name}.gb" for name in (
    "boot_regs-cgb", "boot_hwio-C", "boot_div-cgbABCDE",
    "bits/unused_hwio-C",
))


# Common MBC1/MBC2/MBC5 wiring, including the standard 1-MiB MBC1M variant.
MAPPER_TESTS = tuple(f"emulator-only/{mapper}/{name}.gb"
                     for mapper, names in (
    ("mbc1", ("bits_bank1", "bits_bank2", "bits_mode", "bits_ramg",
              "ram_64kb", "ram_256kb", "rom_512kb", "rom_1Mb",
              "rom_2Mb", "rom_4Mb", "rom_8Mb", "rom_16Mb", "multicart_rom_8Mb")),
    ("mbc2", ("bits_ramg", "bits_romb", "bits_unused", "ram",
              "rom_512kb", "rom_1Mb", "rom_2Mb")),
    ("mbc5", ("rom_512kb", "rom_1Mb", "rom_2Mb", "rom_4Mb",
              "rom_8Mb", "rom_16Mb", "rom_32Mb", "rom_64Mb")),
) for name in names)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--suite-dir", required=True, type=Path,
                        help="Root of an extracted Mooneye test-suite archive")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--group", choices=("cpu", "ppu", "interrupt", "timer", "serial", "dma", "mapper", "cgb", "cgb-boot", "all"), default="ppu",
                        help="Selected regression group (default: ppu)")
    parser.add_argument("--cgb-boot-rom", type=Path,
                        help="External 2304-byte address-indexed CGB boot ROM for cgb-boot checks")
    args = parser.parse_args()
    if args.cgb_boot_rom and args.group not in ("cgb", "cgb-boot"):
        parser.error("--cgb-boot-rom is only valid with --group cgb or cgb-boot")
    if args.group == "cgb-boot" and not args.cgb_boot_rom:
        parser.error("--group cgb-boot requires --cgb-boot-rom")
    if args.cgb_boot_rom and not args.cgb_boot_rom.is_file():
        print(f"CGB boot ROM does not exist: {args.cgb_boot_rom}", file=sys.stderr)
        return 2
    tests = ()
    if args.group in ("cpu", "all"):
        tests += CPU_TESTS
    if args.group in ("ppu", "all"):
        tests += TESTS
    if args.group in ("timer", "all"):
        tests += TIMER_TESTS
    if args.group in ("serial", "all"):
        tests += SERIAL_TESTS
    if args.group in ("dma", "all"):
        tests += DMA_TESTS
    if args.group in ("interrupt", "all"):
        tests += INTERRUPT_TESTS
    if args.group in ("mapper", "all"):
        tests += MAPPER_TESTS
    if args.group == "cgb":
        tests += CGB_TESTS
    if args.group == "cgb-boot":
        tests += CGB_BOOT_TESTS

    missing = [name for name in tests if not (args.suite_dir / name).is_file()]
    if missing:
        print("Missing selected Mooneye ROMs:", file=sys.stderr)
        for name in missing:
            print(f"  {args.suite_dir / name}", file=sys.stderr)
        return 2

    args.output_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    failed = False
    if args.group in ("cgb", "cgb-boot"):
        startup_profile = (f"CGB-E boot ROM ({args.cgb_boot_rom.name})"
                           if args.cgb_boot_rom else "CGB-E synthetic post-boot")
    else:
        startup_profile = "DMG synthetic post-boot"
    for name in tests:
        rom_path = args.suite_dir / name
        # Different mapper groups reuse names such as rom_512kb and bits_ramg.
        # Preserve the suite-relative path so their logs cannot overwrite each other.
        relative_log = Path(name).with_suffix(".log")
        log_path = args.output_dir / relative_log
        log_path.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256(rom_path.read_bytes()).hexdigest()
        started = time.monotonic()
        command = [str(args.runner), str(rom_path)]
        if args.group in ("cgb", "cgb-boot"):
            command += ["--model", "cgb"]
            if args.cgb_boot_rom:
                command += ["--boot-rom", str(args.cgb_boot_rom)]
        result = subprocess.run(
            command,
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
        rows.append((name, status, elapsed, relative_log.as_posix(), digest))
        print(f"{status:4} {name} ({elapsed:.2f}s)")
        if output:
            print(f"     {output.splitlines()[-1]}")

    report = [
        f"# Mooneye timing checks: {args.group}",
        "",
        "Run with the local LotusGB system runner and externally supplied ROMs.",
        "The ROM archive is not copied into this repository.",
        f"Startup profile: {startup_profile}.",
        "",
        "| Test | Result | Runtime | Log | ROM SHA-256 |",
        "| --- | --- | ---: | --- | --- |",
    ]
    report.extend(
        f"| `{name}` | **{status}** | {elapsed:.2f}s | [`{log}`]({log}) | `{digest}` |"
        for name, status, elapsed, log, digest in rows
    )
    report.append("")
    (args.output_dir / "report.md").write_text("\n".join(report), encoding="utf-8")
    print(f"Report: {args.output_dir / 'report.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
