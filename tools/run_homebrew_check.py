#!/usr/bin/env python3
"""Local Big2Small callback/input smoke check; ROM and saves remain external."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--save', type=Path, help='Optional SRAM to restore read-only')
    parser.add_argument('--core', type=Path, default=Path('build/lotusgb_libretro.so'))
    parser.add_argument('--runner', type=Path, default=Path('build/libretro_rom_runner'))
    parser.add_argument('--output-dir', type=Path, default=Path('build/test-roms/big2small'))
    args = parser.parse_args()
    output = args.output_dir.resolve()
    # Prevent any generated artifact from replacing a supplied ROM/save/core/runner.
    artifacts = [output / name for name in ('input.txt', 'frame.rgb', 'frame.ppm',
                                          'output.srm', 'runner.log', 'report.json')]
    for source in (args.rom, args.save, args.core, args.runner):
        if source and any(source.resolve() == target or
                          (target.exists() and source.samefile(target))
                          for target in artifacts):
            parser.error('input path conflicts with an output artifact')
    rom = args.rom.read_bytes()
    if hashlib.sha256(rom).hexdigest() != '59c096333f93c12c8eb25a5fcffa2be15001abcd5d20b9a82c2bc2dae7e625b2':
        parser.error('expected the pinned Big2Small ROM; see docs/homebrew-check.md')
    output.mkdir(parents=True, exist_ok=True)
    events = ['# zero-based start end(exclusive) button']
    events += [f'{start} {start + 5} A' for start in (180, 360, 540)]
    keys = ('RIGHT', 'DOWN', 'LEFT', 'UP')
    for start in range(960, 3600, 240):
        events += [f'{start} {start + 4} A',
                   f'{start + 60} {start + 68} {keys[(start // 240) % 4]}']
    (output / 'input.txt').write_text('\n'.join(events) + '\n')
    original_save_hash = sha256(args.save) if args.save else None
    command = [str(args.runner.resolve()), str(args.core.resolve()),
               str(args.rom.resolve()), '3600', str(output / 'frame.rgb'),
               str(output / 'output.srm'), str(output / 'input.txt')]
    if args.save:
        command.append(str(args.save.resolve()))
    result = subprocess.run(command, capture_output=True, text=True)
    (output / 'runner.log').write_text(result.stdout + result.stderr)
    unchanged = not args.save or sha256(args.save) == original_save_hash
    report = dict(rom_sha256=sha256(args.rom), core_sha256=sha256(args.core),
                  restored_save_sha256=original_save_hash, original_save_unchanged=unchanged,
                  runner_exit_code=result.returncode, command=command,
                  scope='Callback delivery and scripted execution only; inspect frame.ppm manually. '
                        'Not an audio-quality, puzzle-completion or save-persistence assertion.')
    if result.returncode == 0:
        rgb = (output / 'frame.rgb').read_bytes()
        if len(rgb) != 160 * 144 * 3:
            raise RuntimeError('invalid RGB output size')
        (output / 'frame.ppm').write_bytes(b'P6\n160 144\n255\n' + rgb)
        report['frame_sha256'] = hashlib.sha256(rgb).hexdigest()
        report['callbacks'] = result.stdout.strip()
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(result.stdout, end='')
    print(f'Report: {output / "report.json"}')
    if result.returncode or not unchanged:
        raise SystemExit('Homebrew check failed; inspect runner.log and report.json')


if __name__ == '__main__':
    main()
