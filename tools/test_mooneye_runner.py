#!/usr/bin/env python3
"""Exercise the real Mooneye runner with generated result-protocol probes."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def signature(values):
    # LD B,n; LD C,n; LD D,n; LD E,n; LD H,n; LD L,n
    return bytes(value for pair in zip((0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E), values)
                 for value in pair)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', required=True, type=Path)
    args = parser.parse_args()
    passed = signature((3, 5, 8, 13, 21, 34))
    failed = signature((0x42,) * 6)
    marker = bytes((0x40, 0x18, 0xFE))  # LD B,B; JR forever
    cases = (
        ('transient-failure', b'\xAF' + failed + b'\x00' + passed + marker, 0),
        ('transient-success', b'\xAF' + passed + b'\x00' + failed + marker, 1),
        ('real-failure', failed + marker, 1),
        ('cb-opcode-is-not-marker', b'\xAF' + passed + b'\xCB\x40\x00' + failed + marker, 1),
        ('immediate-is-not-marker', b'\xAF' + passed + b'\x3E\x40\x00' + failed + marker, 1),
        ('pass-with-nonzero-A', b'\x3E\x55' + passed + marker, 0),
        ('unrelated-marker', b'\x40\x00' + passed + marker, 0),
    )
    with tempfile.TemporaryDirectory(prefix='lotusgb-mooneye-protocol-') as directory:
        for name, code, expected in cases:
            rom = bytearray(0x8000)
            # Valid ROM-only header; execute generated code beyond the header.
            rom[0x100:0x103] = bytes((0xC3, 0x50, 0x01))
            rom[0x150:0x150 + len(code)] = code
            path = Path(directory) / f'{name}.gb'
            path.write_bytes(rom)
            result = subprocess.run([str(args.runner.resolve()), str(path)],
                                    capture_output=True, text=True, timeout=15)
            output = result.stdout + result.stderr
            expected_report = 'PASS after ' if expected == 0 else 'FAIL signature after '
            if result.returncode != expected or not any(
                    line.startswith(expected_report) for line in output.splitlines()):
                raise SystemExit(f'{name}: expected exit {expected}, got {result.returncode}\n'
                                 f'{result.stdout}{result.stderr}')
            print(f'PASS Mooneye protocol: {name}')


if __name__ == '__main__':
    main()
