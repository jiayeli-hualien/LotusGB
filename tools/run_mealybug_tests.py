#!/usr/bin/env python3
"""Optional DMG visual checks against an external Mealybug source checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

TESTS = (
    'm3_bgp_change',
    'm3_scx_low_3_bits',
    'm3_scx_high_5_bits',
    'm3_scy_change',
    'm3_lcdc_bg_map_change',
    'm3_lcdc_bg_en_change',
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite-dir', type=Path, required=True)
    parser.add_argument('--runner', type=Path, default=Path('build/ppu_frame_runner'))
    parser.add_argument('--output-dir', type=Path, default=Path('build/test-roms/mealybug'))
    parser.add_argument('--test', choices=TESTS, action='append')
    args = parser.parse_args()
    try:
        from PIL import Image
    except ImportError:
        parser.error('this optional visual workflow requires Pillow')
    output = args.output_dir.resolve()
    suite = args.suite_dir.resolve()
    if output == suite or suite in output.parents:
        parser.error('output must stay outside the reference checkout')
    output.mkdir(parents=True, exist_ok=True)
    results = []
    with zipfile.ZipFile(args.suite_dir / 'mealybug-tearoom-tests.zip') as archive:
        for name in args.test or TESTS:
            record = dict(test=name, passed=False)
            try:
                rom = archive.read(name + '.gb')
                rom_path = output / (name + '.gb')
                reference = args.suite_dir / 'expected' / 'DMG-blob' / (name + '.png')
                # All names are fixed choices; never extract arbitrary archive paths.
                rom_path.write_bytes(rom)
                shades_path = output / (name + '.shades')
                shades_path.unlink(missing_ok=True)
                run = subprocess.run([str(args.runner.resolve()), str(rom_path),
                                      str(shades_path)], capture_output=True, text=True, timeout=60)
                (output / (name + '.log')).write_text(run.stdout + run.stderr)
                record.update(rom_sha256=hashlib.sha256(rom).hexdigest(),
                              reference_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
                              runner_exit_code=run.returncode)
                if run.returncode:
                    raise ValueError('capture failed; inspect the runner log')
                shades = shades_path.read_bytes()
                if len(shades) != 160 * 144 or any(value > 3 for value in shades):
                    raise ValueError('invalid captured shade framebuffer')
                image = Image.frombytes('L', (160, 144), bytes(255 - x * 85 for x in shades)).convert('RGB')
                image.save(output / (name + '.png'))
                with Image.open(reference) as source:
                    expected = source.convert('RGB')
                if expected.size != (160, 144):
                    raise ValueError('unexpected reference dimensions')
                actual_bytes, expected_bytes = image.tobytes(), expected.tobytes()
                differences = [actual_bytes[i:i + 3] != expected_bytes[i:i + 3]
                               for i in range(0, len(actual_bytes), 3)]
                record['different_pixels'] = sum(differences)
                record['passed'] = not any(differences)
                diff = Image.new('RGB', (160, 144))
                diff.putdata([(255, 0, 0) if value else (0, 0, 0) for value in differences])
                diff.save(output / (name + '.diff.png'))
            except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
                record['error'] = str(error)
            results.append(record)
            print(('PASS' if record['passed'] else 'FAIL'), name,
                  record.get('different_pixels', record.get('error')), flush=True)
    report = dict(archive_sha256=hashlib.sha256(
                      (args.suite_dir / 'mealybug-tearoom-tests.zip').read_bytes()).hexdigest(),
                  runner_sha256=hashlib.sha256(args.runner.read_bytes()).hexdigest(),
                  reference_model='DMG-blob', results=results,
                  scope='Selected fixed-ROM0 LD B,B visual breakpoints; not full PPU conformance.')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if all(record['passed'] for record in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
