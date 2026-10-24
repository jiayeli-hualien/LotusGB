#!/usr/bin/env python3
"""Local homebrew callback/input smoke checks; ROM and saves remain external."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_run(summary, rgb, save_ram):
    """Check the runner's public output contract for these two profiles."""
    match = re.fullmatch(
        r'frames_requested=(\d+) video_frames=(\d+) audio_frames=(\d+) '
        r'save_ram_bytes=(\d+) width=(\d+) height=(\d+)\s*', summary)
    if not match:
        raise ValueError('missing or malformed runner callback summary')
    requested, video, audio, ram_size, width, height = map(int, match.groups())
    if requested != 3600 or video != requested:
        raise ValueError('expected 3600 video frames')
    if audio == 0:
        raise ValueError('core did not deliver audio frames')
    if (width, height) != (160, 144) or len(rgb) != width * height * 3:
        raise ValueError('invalid RGB output dimensions or size')
    if ram_size != 8192 or len(save_ram) != ram_size:
        raise ValueError('expected an 8192-byte cartridge SRAM output')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', choices=('big2small', 'tobutobugirl'), default='big2small')
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--save', type=Path, help='Optional SRAM to restore read-only')
    parser.add_argument('--core', type=Path, default=Path('build/lotusgb_libretro.so'))
    parser.add_argument('--runner', type=Path, default=Path('build/libretro_rom_runner'))
    parser.add_argument('--output-dir', type=Path, help='Default: build/test-roms/<game>')
    args = parser.parse_args()
    output = (args.output_dir or Path('build/test-roms') / args.game).resolve()
    # Prevent any generated artifact from replacing a supplied ROM/save/core/runner.
    artifacts = [output / name for name in ('input.txt', 'frame.rgb', 'frame.ppm',
                                          'output.srm', 'runner.log', 'report.json')]
    for source in (args.rom, args.save, args.core, args.runner):
        if source and any(source.resolve() == target or
                          (target.exists() and source.samefile(target))
                          for target in artifacts):
            parser.error('input path conflicts with an output artifact')
    rom = args.rom.read_bytes()
    expected_hash = {
        'big2small': '59c096333f93c12c8eb25a5fcffa2be15001abcd5d20b9a82c2bc2dae7e625b2',
        'tobutobugirl': '5d3871cae77db2287807e8914fd21f76203e73aa3fec20955489d45cb4571d8f',
    }[args.game]
    if hashlib.sha256(rom).hexdigest() != expected_hash:
        parser.error(f'expected the pinned {args.game} ROM; see docs/homebrew-check.md')
    output.mkdir(parents=True, exist_ok=True)
    events = ['# zero-based start end(exclusive) button']
    if args.game == 'big2small':
        events += [f'{start} {start + 5} A' for start in (180, 360, 540)]
        keys = ('RIGHT', 'DOWN', 'LEFT', 'UP')
        for start in range(960, 3600, 240):
            events += [f'{start} {start + 4} A',
                       f'{start + 60} {start + 68} {keys[(start // 240) % 4]}']
    else:
        # Skip the opening scenes, confirm a stage, then exercise movement,
        # dash (A) and upward boost (B). No START presses during gameplay.
        events += [f'{start} {start + 5} START' for start in range(180, 1500, 180)]
        events += ['1620 1625 A', '1800 1805 A']
        for start in range(1920, 3600, 120):
            events += [f'{start} {start + 5} A',
                       f'{start + 30} {start + 80} B',
                       f'{start + 30} {start + 70} RIGHT']
    (output / 'input.txt').write_text('\n'.join(events) + '\n')
    original_save_hash = sha256(args.save) if args.save else None
    command = [str(args.runner.resolve()), str(args.core.resolve()),
               str(args.rom.resolve()), '3600', str(output / 'frame.rgb'),
               str(output / 'output.srm'), str(output / 'input.txt')]
    if args.save:
        command.append(str(args.save.resolve()))
    # A successful exit must produce fresh artifacts, never reuse an old run.
    for name in ('frame.rgb', 'frame.ppm', 'output.srm'):
        (output / name).unlink(missing_ok=True)
    result = subprocess.run(command, capture_output=True, text=True)
    (output / 'runner.log').write_text(result.stdout + result.stderr)
    unchanged = not args.save or sha256(args.save) == original_save_hash
    report = dict(game=args.game, rom_sha256=sha256(args.rom), core_sha256=sha256(args.core),
                  restored_save_sha256=original_save_hash, original_save_unchanged=unchanged,
                  runner_exit_code=result.returncode, command=command,
                  scope='Callback delivery and scripted execution only; inspect frame.ppm manually. '
                        'Not an audio-quality, level-completion or save-persistence assertion.')
    validation_error = None
    if result.returncode == 0:
        try:
            rgb = (output / 'frame.rgb').read_bytes()
            save_ram = (output / 'output.srm').read_bytes()
            validate_run(result.stdout, rgb, save_ram)
            (output / 'frame.ppm').write_bytes(b'P6\n160 144\n255\n' + rgb)
            report['frame_sha256'] = hashlib.sha256(rgb).hexdigest()
            report['callbacks'] = result.stdout.strip()
        except (OSError, ValueError) as error:
            validation_error = str(error)
    report['validation_error'] = validation_error
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(result.stdout, end='')
    print(f'Report: {output / "report.json"}')
    if result.returncode or not unchanged or validation_error:
        raise SystemExit('Homebrew check failed; inspect runner.log and report.json')


if __name__ == '__main__':
    main()
