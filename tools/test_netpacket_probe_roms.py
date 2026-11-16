#!/usr/bin/env python3
"""Exercise the original Netpacket probe ROMs through the libretro core."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def run_probe(runner: Path, core: Path, rom: Path, frames: int,
              output_dir: Path, name: str, input_script: Path | None = None,
              restore_sram: Path | None = None) -> bytes:
    image = output_dir / f"{name}.rgb"
    save = output_dir / f"{name}.srm"
    command = [str(runner), str(core), str(rom), str(frames),
               str(image), str(save)]
    if input_script is not None:
        command.append(str(input_script))
    if restore_sram is not None:
        if input_script is None:
            raise ValueError("an input script is required before a restore SRAM")
        command.append(str(restore_sram))
    subprocess.run(command, check=True, capture_output=True, text=True)
    return save.read_bytes()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="lotusgb-netpacket-probe-") as temp:
        root = Path(temp)
        rom_dir = root / "roms"
        subprocess.run(
            [sys.executable, str(args.generator), str(rom_dir),
             "--client-start", "button", "--transfers", "16"],
            check=True, capture_output=True, text=True,
        )
        client_rom = rom_dir / "client.gb"

        before_input = run_probe(
            args.runner, args.core, client_rom, 12, root, "before-input")
        assert before_input[0x10] == 0x11, "client ROM did not reach its input gate"
        assert before_input[0x11] != 0x22, "client sent before Game Boy A was pressed"
        assert before_input[0x12] != 0x33 and before_input[0x13] != 0x44, (
            "client unexpectedly completed a transfer before input"
        )

        input_script = root / "press-a.txt"
        input_script.write_text("5 12 A\n")
        after_input = run_probe(
            args.runner, args.core, client_rom, 48, root, "after-input",
            input_script, root / "before-input.srm")
        assert after_input[0x11] == 0x22, "Game Boy A did not start the transfer"
        assert after_input[0x12] == 0x33 and after_input[0x13] == 0x44, (
            "client did not finish its standalone serial transfer"
        )
        assert after_input[0x14] == 16, "client did not complete all 16 transfers"
        assert after_input[0x20:0x30] == bytes((0xFF,) * 16), (
            "client did not store each received byte in order"
        )

        auto_dir = root / "auto"
        subprocess.run(
            [sys.executable, str(args.generator), str(auto_dir),
             "--transfers", "16"],
            check=True, capture_output=True, text=True,
        )
        auto_client = run_probe(
            args.runner, args.core, auto_dir / "client.gb", 120,
            root, "auto-client")
        assert auto_client[0x11] == 0x22 and auto_client[0x13] == 0x44, (
            "automatic client mode no longer completes without input"
        )
        assert auto_client[0x14] == 16 and auto_client[0x20:0x30] == bytes((0xFF,) * 16), (
            "automatic client mode did not complete its ordered transfer sequence"
        )

    print("Netpacket probe ROMs: PASS (button gate, 16 transfers, auto mode)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
