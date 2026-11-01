#!/usr/bin/env python3
"""Measure pinned external Tobu Tobu Girl / Deluxe ROMs through libretro callbacks."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import tempfile
import zlib


ROM_SHA256 = {
    'tobutobugirl': '5d3871cae77db2287807e8914fd21f76203e73aa3fec20955489d45cb4571d8f',
    'tobutobugirl-dx': '0a0e8018dbbc8d7f8cd99f05e7cdc7b4cc9e358ecfe9377ebfb2291a84c6e310',
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_tobudx_cgb_header(rom):
    """Require the pinned Deluxe release's CGB/MBC1 battery cartridge."""
    if len(rom) <= 0x149:
        raise ValueError('Deluxe ROM is too short to contain its cartridge header')
    if not rom[0x143] & 0x80:
        raise ValueError('Deluxe profile requires a CGB-compatible ROM header')
    if rom[0x147] != 0x03:
        raise ValueError('Deluxe profile requires MBC1 + RAM + battery')
    if rom[0x149] != 0x02:
        raise ValueError('Deluxe profile requires the pinned 8-KiB SRAM geometry')
    return {
        'cgb_header': f'0x{rom[0x143]:02X}',
        'mapper': 'MBC1 + RAM + battery',
        'save_ram_bytes': 8192,
    }


def write_contact_sheet(output):
    """Rows: calls 3/8. Columns: neutral, left, right, B, right+A, right+B."""
    columns = ('neutral', 'left-pulse', 'right-pulse', 'flap-b-pulse',
               'right-dash-pulse', 'right-flap-pulse')
    raw = bytearray()
    for call in (3, 8):
        frames = []
        for column in columns:
            ppm = (output / f'{column}-{call}.ppm').read_bytes()
            header = b'P6\n160 144\n255\n'
            if not ppm.startswith(header) or len(ppm) != len(header) + 160 * 144 * 3:
                raise ValueError('invalid evidence PPM')
            frames.append(ppm[len(header):])
        for y in range(144):
            raw.append(0)  # PNG scanline filter: none
            for frame in frames:
                raw.extend(frame[y * 480:(y + 1) * 480])
    def chunk(kind, data):
        return (struct.pack('!I', len(data)) + kind + data +
                struct.pack('!I', zlib.crc32(kind + data)))
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('!IIBBBBB', 960, 288, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    (output / 'contact-sheet.png').write_bytes(png)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', choices=tuple(ROM_SHA256), default='tobutobugirl')
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--test-executable', type=Path, default=Path('build/libretro_core_test'))
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    deluxe = args.game == 'tobutobugirl-dx'
    output = (args.output_dir or Path('build/test-roms/tobudx-input' if deluxe else 'build/test-roms/tobu-input')).resolve()
    executable = args.test_executable.resolve()
    rom = args.rom.resolve()
    for source in (rom, executable):
        if source == output or output in source.parents:
            parser.error('ROM and test executable must stay outside the output directory')
    if digest(rom) != ROM_SHA256[args.game]:
        parser.error('expected the pinned game ROM; see docs/homebrew-check.md and docs/cgb-development.md')
    cartridge = None
    if deluxe:
        try:
            cartridge = validate_tobudx_cgb_header(rom.read_bytes())
        except ValueError as error:
            parser.error(str(error))
    output.mkdir(parents=True, exist_ok=True)
    for name in ('report.json', 'measurements.json', 'contact-sheet.png'):
        (output / name).unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='run-', dir=output) as temporary:
        staging = Path(temporary)
        command = [str(executable), '--tobudx-check' if deluxe else '--tobu-input', str(rom), str(staging)]
        run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (staging / 'runner.log').write_text(run.stdout)
        (staging / 'runner.log').replace(output / 'runner.log')
        print(run.stdout, end='')
        run.check_returncode()
        for artifact in staging.iterdir():
            artifact.replace(output / artifact.name)
    measurements = json.loads((output / 'measurements.json').read_text())
    write_contact_sheet(output)
    rows = measurements['results']
    if len(rows) != 72:
        raise ValueError('expected twelve checkpoints for each of six actions')
    summary = {}
    for action in sorted({row['action'] for row in rows}):
        selected = [row for row in rows if row['action'] == action]
        summary[action] = {
            key: dict(sorted(Counter(row[key] for row in selected).items()))
            for key in ('held_press_call', 'pulse_press_call', 'release_difference_call',
                        'compound_effect_call')
        }
    report = {
        'game': args.game, 'hardware': 'CGB-E' if deluxe else 'DMG',
        'rom_sha256': digest(rom),
        'test_executable_sha256': digest(executable),
        'pixel_format': 'XRGB8888', 'startup': 'fast post-boot, no BIOS, fresh SRAM',
        'frame_step': measurements['step_mode'],
        'command': command, 'summary': summary, 'measurements': measurements,
        'scope': ('Core/game visible response in a central playfield region; call 1 '
                  'includes the press poll. Value 0 means no visible difference '
                  'within the observation window, not zero latency. Release compares '
                  'one-poll and held input; A without direction is diagnostic only. '
                  'No physical input/display latency or frontend feel claim.'),
    }
    if cartridge:
        report['cartridge'] = cartridge
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
