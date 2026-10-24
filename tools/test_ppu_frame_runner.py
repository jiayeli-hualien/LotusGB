#!/usr/bin/env python3
"""ROM-free checks for visual breakpoint capture."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', type=Path, required=True)
    args = parser.parse_args()
    cases = (
        ('marker', bytes([0x40, 0x18, 0xFE]), 0, 'CAPTURE'),
        ('cb-is-not-marker', bytes([0xCB, 0x40, 0x18, 0xFC]), 1, 'TIMEOUT'),
        ('immediate-is-not-marker', bytes([0x3E, 0x40, 0x18, 0xFC]), 1, 'TIMEOUT'),
        ('stop', bytes([0x10, 0x00]), 1, 'CPU failed or stopped'),
    )
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        for name, program, code, tag in cases:
            rom = bytearray(0x8000)
            rom[0x100:0x100 + len(program)] = program
            path = root / (name + '.gb')
            output = root / (name + '.shades')
            path.write_bytes(rom)
            result = subprocess.run([str(args.runner.resolve()), str(path), str(output)],
                                    capture_output=True, text=True, timeout=30)
            assert result.returncode == code, (name, result.returncode, result.stderr)
            assert tag in result.stdout + result.stderr, (name, result.stdout, result.stderr)
            if code == 0:
                assert output.read_bytes() == bytes(160 * 144), name
            else:
                assert not output.exists(), name
            if name == 'marker':
                rejected = subprocess.run([str(args.runner.resolve()), str(path), str(path)],
                                          capture_output=True, text=True, timeout=30)
                assert rejected.returncode == 2 and path.read_bytes() == rom
            print('PASS visual capture:', name)


if __name__ == '__main__':
    main()
