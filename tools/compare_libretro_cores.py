#!/usr/bin/env python3
"""Run the same ROM/input script through two local libretro cores."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zlib


WIDTH = 160
HEIGHT = 144
FRAME_BYTES = WIDTH * HEIGHT * 3
SUMMARY = re.compile(
    r"frames_requested=(\d+) video_frames=(\d+) audio_frames=(\d+) "
    r"save_ram_bytes=(\d+) width=(\d+) height=(\d+)\s*"
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def png_chunk(kind: bytes, data: bytes) -> bytes:
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    if len(rgb) != width * height * 3:
        raise ValueError(f"invalid RGB image size for {path}")
    scanlines = b"".join(
        b"\x00" + rgb[row * width * 3:(row + 1) * width * 3]
        for row in range(height)
    )
    png = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(scanlines, level=9))
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def run_core(label: str, core: Path, rom: Path, runner: Path, frames: int,
             input_script: Path | None, system_directory: Path | None,
             output: Path) -> dict:
    rgb_path = output / f"{label}.rgb"
    sram_path = output / f"{label}.srm"
    log_path = output / f"{label}.log"
    for path in (rgb_path, sram_path, log_path, output / f"{label}.png"):
        path.unlink(missing_ok=True)

    command = [str(runner), str(core), str(rom), str(frames),
               str(rgb_path), str(sram_path)]
    if input_script:
        command.append(str(input_script))
    child_env = os.environ.copy()
    child_env.pop("LOTUSGB_SYSTEM_DIRECTORY", None)
    if system_directory:
        child_env["LOTUSGB_SYSTEM_DIRECTORY"] = str(system_directory)
    with tempfile.TemporaryDirectory(prefix=f"{label}-cwd-", dir=output) as cwd:
        result = subprocess.run(command, capture_output=True, text=True,
                                check=False, cwd=cwd, env=child_env)
    log_path.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"{label} runner failed ({result.returncode}); see {log_path}")

    match = SUMMARY.fullmatch(result.stdout)
    if not match:
        raise ValueError(f"{label} runner returned a malformed summary; see {log_path}")
    requested, video, audio, sram_size, width, height = map(int, match.groups())
    if requested != frames or video != frames or (width, height) != (WIDTH, HEIGHT):
        raise ValueError(f"{label} returned unexpected frame metadata: {result.stdout.strip()}")
    rgb = rgb_path.read_bytes()
    if len(rgb) != FRAME_BYTES:
        raise ValueError(f"{label} returned {len(rgb)} RGB bytes, expected {FRAME_BYTES}")
    sram = sram_path.read_bytes()
    if len(sram) != sram_size:
        raise ValueError(f"{label} returned {len(sram)} SRAM bytes, reported {sram_size}")
    write_png(output / f"{label}.png", WIDTH, HEIGHT, rgb)
    return {
        "core_path": str(core),
        "core_sha256": sha256(core.read_bytes()),
        "runner_summary": result.stdout.strip(),
        "frame_sha256": sha256(rgb),
        "sram_sha256": sha256(sram),
        "audio_frames": audio,
        "sram_bytes": sram_size,
        "log": log_path.name,
        "image": f"{label}.png",
    }, rgb


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--core-a", required=True, type=Path,
                        help="First core, usually LotusGB")
    parser.add_argument("--core-b", required=True, type=Path,
                        help="Comparison core, such as SameBoy")
    parser.add_argument("--runner", type=Path, default=Path("build/libretro_rom_runner"))
    parser.add_argument("--frames", required=True, type=int)
    parser.add_argument("--inputs", type=Path,
                        help="Optional zero-based input script accepted by libretro_rom_runner")
    parser.add_argument("--system-directory", type=Path,
                        help="Optional libretro system directory, for example one containing dmg_boot.bin")
    parser.add_argument("--output-dir", type=Path,
                        default=Path("build/test-roms/core-comparison"))
    parser.add_argument("--require-identical", action="store_true",
                        help="return failure if the final RGB frames differ")
    args = parser.parse_args()

    if args.frames <= 0:
        parser.error("--frames must be positive")
    rom = args.rom.resolve()
    core_a = args.core_a.resolve()
    core_b = args.core_b.resolve()
    runner = args.runner.resolve()
    inputs = args.inputs.resolve() if args.inputs else None
    system_directory = args.system_directory.resolve() if args.system_directory else None
    output = args.output_dir.resolve()
    for path in (rom, core_a, core_b, runner, inputs):
        if path is not None and not path.is_file():
            parser.error(f"file does not exist: {path}")
    if system_directory is not None and not system_directory.is_dir():
        parser.error(f"system directory does not exist: {system_directory}")
    for source in (rom, core_a, core_b, runner, inputs):
        if source and (output == source or output in source.parents):
            parser.error("output directory must not contain ROM, core, runner, or input files")
    if core_a == core_b:
        parser.error("--core-a and --core-b must refer to different files")

    output.mkdir(parents=True, exist_ok=True)
    for name in ("report.json", "comparison.png", "core-a.rgb", "core-b.rgb",
                 "core-a.srm", "core-b.srm", "core-a.png", "core-b.png",
                 "core-a.log", "core-b.log"):
        (output / name).unlink(missing_ok=True)

    report = {
        "rom_path": str(rom),
        "rom_sha256": sha256(rom.read_bytes()),
        "runner_path": str(runner),
        "runner_sha256": sha256(runner.read_bytes()),
        "input_path": str(inputs) if inputs else None,
        "input_sha256": sha256(inputs.read_bytes()) if inputs else None,
        "system_directory": str(system_directory) if system_directory else None,
        "frames": args.frames,
        "firmware_supplied": [],
        "working_directory_policy": (
            "Each core runs in a fresh empty temporary directory so a local "
            "relative-path boot ROM cannot change its startup mode."
        ),
        "scope": "Local cross-core diagnostic only; neither core is treated as a hardware oracle.",
    }
    try:
        result_a, rgb_a = run_core("core-a", core_a, rom, runner,
                                   args.frames, inputs, system_directory,
                                   output)
        result_b, rgb_b = run_core("core-b", core_b, rom, runner,
                                   args.frames, inputs, system_directory,
                                   output)
        differing = sum(rgb_a[offset:offset + 3] != rgb_b[offset:offset + 3]
                        for offset in range(0, FRAME_BYTES, 3))
        diff = bytearray(FRAME_BYTES)
        for offset, (a, b) in enumerate(zip(rgb_a, rgb_b)):
            diff[offset] = min(255, abs(a - b) * 4)
        sheet = bytearray(HEIGHT * WIDTH * 3 * 3)
        row_bytes = WIDTH * 3
        sheet_row_bytes = WIDTH * 3 * 3
        for row in range(HEIGHT):
            src = row * row_bytes
            dst = row * sheet_row_bytes
            sheet[dst:dst + row_bytes] = rgb_a[src:src + row_bytes]
            sheet[dst + row_bytes:dst + 2 * row_bytes] = rgb_b[src:src + row_bytes]
            sheet[dst + 2 * row_bytes:dst + 3 * row_bytes] = diff[src:src + row_bytes]
        write_png(output / "comparison.png", WIDTH * 3, HEIGHT, bytes(sheet))
        report.update({
            "core_a": result_a,
            "core_b": result_b,
            "differing_pixels": differing,
            "identical_rgb": differing == 0,
            "comparison_image": "comparison.png",
            "comparison_panels": ["core-a", "core-b", "4x absolute RGB difference"],
        })
    except (OSError, RuntimeError, ValueError) as error:
        report["error"] = str(error)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"Core comparison failed: {error}", file=sys.stderr)
        print(f"Report: {output / 'report.json'}", file=sys.stderr)
        return 1

    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Differing pixels: {differing} / {WIDTH * HEIGHT}")
    print(f"Report: {output / 'report.json'}")
    print(f"Comparison image: {output / 'comparison.png'}")
    return int(args.require_identical and differing != 0)


if __name__ == "__main__":
    raise SystemExit(main())
