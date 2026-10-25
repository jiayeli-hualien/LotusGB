#!/usr/bin/env python3
"""Localize the selected SCX discrepancy with controlled external ROM variants.

A completed diagnostic is not a hardware-conformance pass. Comparison-core
output is calibrated against the upstream image, then used only for localization.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


ROM_SHA256 = '423db91af0f87547ae46690044ef760481f7005033ffc61bd69875b7df96139b'
SIZE = (160, 144)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def variants(original):
    if hashlib.sha256(original).hexdigest() != ROM_SHA256:
        raise ValueError('expected the pinned upstream m3_scx_high_5_bits ROM')
    setup = bytes.fromhex('3e93e040')  # LD A,$93; LDH ($40),A.
    table = bytes(v for i in range(18) for v in (16 + 8 * i, i, 0, 0))
    if original.count(setup) != 1 or original.count(table) != 1:
        raise ValueError('ambiguous LCDC setup or sprite table')
    lcdc_offset = original.index(setup) + 1
    table_offset = original.index(table)
    if min(lcdc_offset, table_offset) <= 0x14F:
        raise ValueError('diagnostic patch would touch the cartridge header')
    no_objects = bytearray(original)
    no_objects[lcdc_offset] = 0x91
    result = {'obj_on': original, 'obj_off': bytes(no_objects)}
    for x in (1, 8):
        single = bytearray(original)
        for i in range(18):
            single[table_offset + i * 4] = 0  # Outside visible Y range.
        single[table_offset + 4] = 24  # One object on LY 8 through 15.
        single[table_offset + 5] = x
        result[f'single_x{x}'] = bytes(single)
    return result


def run(command, log):
    process = subprocess.run([str(arg) for arg in command], capture_output=True,
                             text=True, timeout=60)
    log.write_text(process.stdout + process.stderr)
    if process.returncode:
        raise ValueError(f'capture failed with exit {process.returncode}: {log}')
    return process.stdout.strip()


def differences(first, second):
    if len(first) != SIZE[0] * SIZE[1] * 3 or len(first) != len(second):
        raise ValueError('invalid RGB framebuffer dimensions')
    return [first[i:i + 3] != second[i:i + 3]
            for i in range(0, len(first), 3)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', required=True, type=Path)
    parser.add_argument('--reference', required=True, type=Path)
    parser.add_argument('--comparison-core', required=True, type=Path)
    parser.add_argument('--ppu-runner', type=Path, default=Path('build/ppu_frame_runner'))
    parser.add_argument('--libretro-runner', type=Path, default=Path('build/libretro_rom_runner'))
    parser.add_argument('--output-dir', type=Path, default=Path('build/test-roms/ppu-scx-decomposition'))
    args = parser.parse_args()
    try:
        from PIL import Image
    except ImportError:
        parser.error('this optional visual diagnostic requires Pillow')
    output = args.output_dir.resolve()
    inputs = (args.rom, args.reference, args.comparison_core,
              args.ppu_runner, args.libretro_runner)
    if any(output == path.resolve() or output in path.resolve().parents for path in inputs):
        parser.error('output must not contain any input file')
    output.mkdir(parents=True, exist_ok=True)
    report = {'completed': False, 'scope': __doc__, 'results': []}
    try:
        report['inputs'] = {str(path): sha256(path) for path in inputs}
        library = args.ppu_runner.resolve().parent / 'liblotusgb.so'
        report['lotus_library_sha256'] = sha256(library)
        with Image.open(args.reference) as image:
            if image.size != SIZE:
                raise ValueError('unexpected upstream image dimensions')
            reference = image.convert('RGB').tobytes()
        original = args.rom.read_bytes()
        report['comparison_capture_frames'] = [120, 180]
        for name, data in variants(original).items():
            rom = output / (name + '.gb')
            rom.write_bytes(data)
            shades = output / (name + '.shades')
            shades.unlink(missing_ok=True)
            capture = run((args.ppu_runner.resolve(), rom, shades),
                          output / (name + '.log'))
            shade_data = shades.read_bytes()
            if len(shade_data) != SIZE[0] * SIZE[1] or any(v > 3 for v in shade_data):
                raise ValueError('invalid LotusGB shade framebuffer')
            actual = bytes(c for v in shade_data for c in (255 - v * 85,) * 3)
            comparison_frames = []
            for frames in (120, 180):
                prefix = output / f'{name}-comparison-{frames}'
                rgb = prefix.with_suffix('.rgb')
                save = prefix.with_suffix('.srm')
                rgb.unlink(missing_ok=True)
                save.unlink(missing_ok=True)
                run((args.libretro_runner.resolve(), args.comparison_core.resolve(),
                     rom, frames, rgb, save), prefix.with_suffix('.log'))
                comparison_frames.append(rgb.read_bytes())
            if comparison_frames[0] != comparison_frames[1]:
                raise ValueError(f'{name}: comparison frame is not stable')
            expected = comparison_frames[0]
            delta = differences(actual, expected)
            if name == 'obj_on' and any(differences(expected, reference)):
                raise ValueError('comparison core does not reproduce the upstream baseline')
            row = {'variant': name, 'rom_sha256': sha256(rom), 'capture': capture,
                   'patches': [{'offset': i, 'before': a, 'after': b}
                               for i, (a, b) in enumerate(zip(original, data)) if a != b],
                   'lotus_shades_sha256': sha256(shades),
                   'comparison_rgb_sha256': hashlib.sha256(expected).hexdigest(),
                   'different_pixels': sum(delta),
                   'different_x': sorted({i % SIZE[0] for i, v in enumerate(delta) if v}),
                   'different_y': sorted({i // SIZE[0] for i, v in enumerate(delta) if v})}
            if name == 'obj_on':
                row['comparison_upstream_different_pixels'] = 0
            report['results'].append(row)
            Image.frombytes('RGB', SIZE, actual).save(output / (name + '.png'))
            Image.frombytes('RGB', SIZE, expected).save(output / (name + '-comparison.png'))
            diff = bytes(c for v in delta for c in ((255, 0, 0) if v else (0, 0, 0)))
            Image.frombytes('RGB', SIZE, diff).save(output / (name + '-diff.png'))
            print(f'DIAGNOSTIC {name}: {sum(delta)} differing pixels', flush=True)
        report['patch_scope'] = ('Program LCDC.1 immediate or sprite-table bytes only; '
                                 'instruction flow and header unchanged. '
                                 'Global ROM checksum is not repaired.')
        report['completed'] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report['error'] = str(error)
        print(f'ERROR: {error}', flush=True)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['completed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
