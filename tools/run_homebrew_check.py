#!/usr/bin/env python3
"""Local homebrew callback/input/progress checks; ROMs and saves stay external."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess


ADJUSTRIS_CAPTURE_START = 1000
ADJUSTRIS_CAPTURE_END = 1140
FRAME_WIDTH = 160
FRAME_HEIGHT = 144
RGB_BYTES_PER_FRAME = FRAME_WIDTH * FRAME_HEIGHT * 3
BIG2SMALL_CAPTURE_START = 2590
BIG2SMALL_CAPTURE_END = 2591
BIG2SMALL_TITLE_FRAME = 599
BIG2SMALL_RESUME_CAPTURE_START = 2199
BIG2SMALL_RESUME_CAPTURE_END = 2301


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_run(summary, rgb, save_ram):
    """Check the runner's public output contract for the pinned profiles."""
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


def adjustris_occupied_cells(frame_rgb):
    """Read DMG block tiles from the pinned 160x144 Adjustris frame."""
    if len(frame_rgb) != RGB_BYTES_PER_FRAME:
        raise ValueError('invalid Adjustris frame size')
    occupied = set()
    for row in range(18):
        for column in range(10):
            pixel = ((row * 8 + 4) * FRAME_WIDTH + 16 + column * 8 + 4) * 3
            if frame_rgb[pixel] < 220:
                occupied.add((row, column))
    return occupied


def validate_adjustris_observations(observations):
    """Require a completed bottom row to clear and the next piece to move."""
    if not observations:
        raise ValueError('Adjustris progress capture is empty')
    full_row = {(17, column) for column in range(10)}
    full_index = next((i for i, (_, cells) in enumerate(observations)
                       if full_row.issubset(cells)), None)
    if full_index is None:
        raise ValueError('script did not visibly fill one complete row')
    full_frame = observations[full_index][0]
    cleared = next(((i, frame) for i, (frame, cells) in enumerate(observations)
                    if i > full_index and 35 <= frame - full_frame <= 90 and
                    cells.isdisjoint(full_row)), None)
    if cleared is None:
        raise ValueError('completed bottom row did not visibly clear')
    clear_index, clear_frame = cleared

    active = []
    for i in range(clear_index + 1, len(observations)):
        frame, cells = observations[i]
        if frame - clear_frame > 35:
            break
        upper_cells = [(row, column) for row, column in cells if row < 17]
        if len(upper_cells) == 1:
            active.append((frame, upper_cells[0]))
    if not active:
        raise ValueError('no controllable piece appeared after the row cleared')
    before_frame, (before_row, before_column) = active[0]
    moved = next(((frame, row, column) for frame, (row, column) in active[1:]
                  if row == before_row and column > before_column), None)
    if moved is None:
        raise ValueError('next piece did not respond to right input after the clear')
    after_frame, after_row, after_column = moved
    return {
        'full_row_frame': full_frame,
        'row_cleared_frame': clear_frame,
        'next_piece_control_before_frame': before_frame,
        'next_piece_control_after_frame': after_frame,
        'next_piece_control_start_cell': [before_row, before_column],
        'next_piece_control_end_cell': [after_row, after_column],
    }


def inspect_adjustris_progress(sequence_path, start_frame, end_frame):
    """Check captured game frames for a row clear and post-clear movement."""
    sequence = sequence_path.read_bytes()
    frame_count = end_frame - start_frame
    if frame_count <= 0 or len(sequence) != frame_count * RGB_BYTES_PER_FRAME:
        raise ValueError('Adjustris progress capture has an invalid size')
    observations = []
    for offset in range(frame_count):
        start = offset * RGB_BYTES_PER_FRAME
        frame = sequence[start:start + RGB_BYTES_PER_FRAME]
        observations.append((start_frame + offset,
                             adjustris_occupied_cells(frame)))
    return validate_adjustris_observations(observations)


def differing_pixel_count(first, second):
    if len(first) != len(second) or len(first) != 160 * 144 * 3:
        raise ValueError('cannot compare frames with invalid RGB sizes')
    return sum(first[index:index + 3] != second[index:index + 3]
               for index in range(0, len(first), 3))


def captured_frame(sequence_path, start_frame, end_frame, frame_number):
    """Return one RGB frame from a validated raw frame sequence."""
    sequence = sequence_path.read_bytes()
    frame_count = end_frame - start_frame
    if frame_count <= 0 or len(sequence) != frame_count * RGB_BYTES_PER_FRAME:
        raise ValueError('captured frame sequence has an invalid size')
    if not start_frame <= frame_number < end_frame:
        raise ValueError('requested frame is outside the captured interval')
    offset = (frame_number - start_frame) * RGB_BYTES_PER_FRAME
    return sequence[offset:offset + RGB_BYTES_PER_FRAME]


def validate_big2small_save_cycle(empty_save, completed_save, empty_title,
                                  completed_title, before_input, after_input):
    """Require saved puzzle progress and visible control after a fresh load."""
    if len(empty_save) != 8192 or len(completed_save) != 8192:
        raise ValueError('Big2Small save-cycle SRAM must be 8192 bytes')
    changed_save_bytes = sum(a != b for a, b in zip(empty_save, completed_save))
    if changed_save_bytes == 0:
        raise ValueError('completed puzzle did not change cartridge SRAM')
    title_pixels = differing_pixel_count(empty_title, completed_title)
    if title_pixels < 10:
        raise ValueError('restored title did not visibly differ from the 00% save')
    input_pixels = differing_pixel_count(before_input, after_input)
    if input_pixels < 100:
        raise ValueError('restored gameplay did not visibly respond to right input')
    return {
        'sram_bytes_changed_from_empty_slot': changed_save_bytes,
        'empty_vs_restored_title_differing_pixels': title_pixels,
        'post_restore_input_differing_pixels': input_pixels,
    }


def write_events(path, events):
    path.write_text('\n'.join(events) + ('\n' if events else ''))


def run_auxiliary_profile(args, output, label, input_path, restore_path=None,
                          capture_start=None, capture_end=None):
    """Run and validate an additional independent 3,600-frame process."""
    frame_path = output / f'{label}-frame.rgb'
    save_path = output / f'{label}.srm'
    log_path = output / f'{label}-runner.log'
    command = [str(args.runner.resolve()), str(args.core.resolve()),
               str(args.rom.resolve()), '3600', str(frame_path),
               str(save_path), str(input_path)]
    if restore_path:
        command.append(str(restore_path))

    environment = os.environ.copy()
    for capture_variable in ('LOTUSGB_CAPTURE_SEQUENCE',
                             'LOTUSGB_CAPTURE_START', 'LOTUSGB_CAPTURE_END'):
        environment.pop(capture_variable, None)
    sequence_path = None
    if capture_start is not None or capture_end is not None:
        if capture_start is None or capture_end is None:
            raise ValueError('capture start and end must be supplied together')
        sequence_path = output / f'{label}-sequence.rgb'
        environment.update({
            'LOTUSGB_CAPTURE_SEQUENCE': str(sequence_path),
            'LOTUSGB_CAPTURE_START': str(capture_start),
            'LOTUSGB_CAPTURE_END': str(capture_end),
        })
    result = subprocess.run(command, capture_output=True, text=True,
                            env=environment)
    (output / f'{label}-runner.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise ValueError(f'{label} runner failed; inspect {log_path.name}')
    rgb = frame_path.read_bytes()
    save_ram = save_path.read_bytes()
    validate_run(result.stdout, rgb, save_ram)
    return {
        'command': command,
        'callbacks': result.stdout.strip(),
        'frame': rgb,
        'save_ram': save_ram,
        'sequence_path': sequence_path,
        'save_path': save_path,
    }


def inspect_big2small_save_cycle(args, output, completed_save):
    """Compare an empty 00% slot to a completed save, then play after reload."""
    completion_rgb = captured_frame(
        output / 'progress-sequence.rgb', BIG2SMALL_CAPTURE_START,
        BIG2SMALL_CAPTURE_END, 2590)
    empty_slot_events = ['180 185 A', '360 365 A', '540 545 A', '720 725 A']
    write_events(output / 'empty-slot-input.txt', empty_slot_events)
    empty_slot = run_auxiliary_profile(
        args, output, 'empty-slot', output / 'empty-slot-input.txt')

    neutral_input = output / 'neutral-input.txt'
    write_events(neutral_input, [])
    empty_title = run_auxiliary_profile(
        args, output, 'empty-title', neutral_input,
        restore_path=empty_slot['save_path'],
        capture_start=BIG2SMALL_TITLE_FRAME,
        capture_end=BIG2SMALL_TITLE_FRAME + 1)
    restored_title = run_auxiliary_profile(
        args, output, 'resume-title', neutral_input,
        restore_path=completed_save,
        capture_start=BIG2SMALL_TITLE_FRAME,
        capture_end=BIG2SMALL_TITLE_FRAME + 1)

    gameplay_events = ['630 635 A', '900 905 A',
                       '1500 1505 A', '1600 1605 A', '1700 1705 A',
                       '2200 2208 RIGHT']
    write_events(output / 'resume-gameplay-input.txt', gameplay_events)
    restored_gameplay = run_auxiliary_profile(
        args, output, 'resume-gameplay',
        output / 'resume-gameplay-input.txt', restore_path=completed_save,
        capture_start=BIG2SMALL_RESUME_CAPTURE_START,
        capture_end=BIG2SMALL_RESUME_CAPTURE_END)

    empty_title_rgb = captured_frame(
        empty_title['sequence_path'], BIG2SMALL_TITLE_FRAME,
        BIG2SMALL_TITLE_FRAME + 1, BIG2SMALL_TITLE_FRAME)
    restored_title_rgb = captured_frame(
        restored_title['sequence_path'], BIG2SMALL_TITLE_FRAME,
        BIG2SMALL_TITLE_FRAME + 1, BIG2SMALL_TITLE_FRAME)
    before_input = captured_frame(
        restored_gameplay['sequence_path'], BIG2SMALL_RESUME_CAPTURE_START,
        BIG2SMALL_RESUME_CAPTURE_END, BIG2SMALL_RESUME_CAPTURE_START)
    after_input_frame = BIG2SMALL_RESUME_CAPTURE_END - 1
    after_input = captured_frame(
        restored_gameplay['sequence_path'], BIG2SMALL_RESUME_CAPTURE_START,
        BIG2SMALL_RESUME_CAPTURE_END, after_input_frame)
    metrics = validate_big2small_save_cycle(
        empty_slot['save_ram'], completed_save.read_bytes(),
        empty_title_rgb, restored_title_rgb, before_input, after_input)

    (output / 'empty-title.ppm').write_bytes(
        b'P6\n160 144\n255\n' + empty_title_rgb)
    (output / 'resume-title.ppm').write_bytes(
        b'P6\n160 144\n255\n' + restored_title_rgb)
    (output / 'puzzle-completed.ppm').write_bytes(
        b'P6\n160 144\n255\n' + completion_rgb)
    (output / 'resume-gameplay-before.ppm').write_bytes(
        b'P6\n160 144\n255\n' + before_input)
    (output / 'resume-gameplay-after.ppm').write_bytes(
        b'P6\n160 144\n255\n' + after_input)

    return {
        'result': 'passed',
        'completion_input': [
            '180-185 A: open save-file menu',
            '360-365 A: create/select file A',
            '540-545 A: select Peanut Pathway',
            '720-725 A: enter puzzle',
            '1500/1600/1700 A: dismiss the introduction',
            '2200 RIGHT, 2300 UP, 2400 RIGHT: solve Peanut Pathway',
            '2600/2750 A: dismiss completion and return to the map',
        ],
        'empty_slot_sram_sha256': sha256(empty_slot['save_path']),
        'completed_sram_sha256': sha256(completed_save),
        'completion_frame': 2590,
        'completion_frame_sha256': hashlib.sha256(completion_rgb).hexdigest(),
        'empty_title_frame': BIG2SMALL_TITLE_FRAME,
        'empty_title_frame_sha256': hashlib.sha256(empty_title_rgb).hexdigest(),
        'restored_title_frame_sha256': hashlib.sha256(
            restored_title_rgb).hexdigest(),
        'restored_title_expected_progress': (
            '02% (review resume-title.ppm; checker compares against 00%)'),
        'restored_gameplay_before_input_frame': BIG2SMALL_RESUME_CAPTURE_START,
        'restored_gameplay_after_input_frame': after_input_frame,
        'restored_gameplay_input': 'RIGHT',
        'metrics': metrics,
        'callbacks': {
            'empty_slot': empty_slot['callbacks'],
            'empty_title_restore': empty_title['callbacks'],
            'completed_title_restore': restored_title['callbacks'],
            'gameplay_restore': restored_gameplay['callbacks'],
        },
        'input_files': {
            'completed_progress': 'input.txt',
            'empty_slot': 'empty-slot-input.txt',
            'title_restores': 'neutral-input.txt',
            'gameplay_restore': 'resume-gameplay-input.txt',
        },
        'commands': {
            'empty_slot': empty_slot['command'],
            'empty_title_restore': empty_title['command'],
            'completed_title_restore': restored_title['command'],
            'gameplay_restore': restored_gameplay['command'],
        },
        'scope': ('Independent LotusGB runner/core SRAM round trip for one completed '
                  'Big2Small puzzle. The 00% and 02% title frames are compared, then '
                  'directional input is checked after restoring the completed save. '
                  'This does not validate RetroArch disk persistence or hardware.'),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', choices=('big2small', 'tobutobugirl', 'canyon-racer', 'adjustris'), default='big2small')
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--save', type=Path, help='Optional SRAM to restore read-only')
    parser.add_argument('--core', type=Path, default=Path('build/lotusgb_libretro.so'))
    parser.add_argument('--runner', type=Path, default=Path('build/libretro_rom_runner'))
    parser.add_argument('--output-dir', type=Path, help='Default: build/test-roms/<game>')
    args = parser.parse_args()
    output = (args.output_dir or Path('build/test-roms') / args.game).resolve()
    # Prevent any generated artifact from replacing a supplied ROM/save/core/runner.
    artifacts = [output / name for name in (
        'input.txt', 'frame.rgb', 'frame.ppm', 'output.srm', 'runner.log',
        'report.json', 'baseline-input.txt', 'baseline-frame.rgb',
        'baseline-frame.ppm', 'baseline-output.srm', 'baseline-runner.log',
        'progress-sequence.rgb', 'baseline-progress-sequence.rgb',
        'progress-full-row.ppm',
        'progress-row-cleared.ppm', 'progress-next-piece-moved.ppm',
        'empty-slot-input.txt', 'empty-slot-frame.rgb', 'empty-slot.srm',
        'empty-slot-runner.log', 'empty-title-frame.rgb', 'empty-title.srm',
        'empty-title-runner.log', 'empty-title-sequence.rgb', 'empty-title.ppm',
        'resume-title-frame.rgb', 'resume-title.srm',
        'resume-title-runner.log', 'resume-title-sequence.rgb',
        'resume-title.ppm', 'neutral-input.txt', 'puzzle-completed.ppm',
        'resume-gameplay-input.txt', 'resume-gameplay-frame.rgb',
        'resume-gameplay.srm', 'resume-gameplay-runner.log',
        'resume-gameplay-sequence.rgb', 'resume-gameplay-before.ppm',
        'resume-gameplay-after.ppm')]
    for source in (args.rom, args.save, args.core, args.runner):
        if source and any(source.resolve() == target or
                          (target.exists() and source.samefile(target))
                          for target in artifacts):
            parser.error('input path conflicts with an output artifact')
    rom = args.rom.read_bytes()
    expected_hash = {
        'big2small': '59c096333f93c12c8eb25a5fcffa2be15001abcd5d20b9a82c2bc2dae7e625b2',
        'tobutobugirl': '5d3871cae77db2287807e8914fd21f76203e73aa3fec20955489d45cb4571d8f',
        'canyon-racer': '19f57c1fefe8ebdcef0ff9def783246a6618d4b822b5c44188a1e9c1e7448c51',
        'adjustris': '9b7bfcd8cec7d85e257c43cc62a9965852d59c9fe94dd9f643de3f225450b58b',
    }[args.game]
    if hashlib.sha256(rom).hexdigest() != expected_hash:
        parser.error(f'expected the pinned {args.game} ROM; see docs/homebrew-check.md')
    output.mkdir(parents=True, exist_ok=True)
    events = ['# zero-based start end(exclusive) button']
    if args.game == 'big2small':
        if args.save:
            # Keep the optional player-save inspection profile read-only.
            events += [f'{start} {start + 5} A' for start in (180, 360, 540)]
            keys = ('RIGHT', 'DOWN', 'LEFT', 'UP')
            for start in range(960, 3600, 240):
                events += [f'{start} {start + 4} A',
                           f'{start + 60} {start + 68} '
                           f'{keys[(start // 240) % 4]}']
        else:
            # Create a fresh file, complete Peanut Pathway, then return to the map.
            events += ['180 185 A', '360 365 A', '540 545 A', '720 725 A',
                       '1500 1505 A', '1600 1605 A', '1700 1705 A',
                       '2200 2208 RIGHT', '2300 2308 UP', '2400 2408 RIGHT',
                       '2600 2605 A', '2750 2755 A']
    elif args.game == 'tobutobugirl':
        # Skip the opening scenes, confirm a stage, then exercise movement,
        # dash (A) and upward boost (B). No START presses during gameplay.
        events += [f'{start} {start + 5} START' for start in range(180, 1500, 180)]
        events += ['1620 1625 A', '1800 1805 A']
        for start in range(1920, 3600, 120):
            events += [f'{start} {start + 5} A',
                       f'{start + 30} {start + 80} B',
                       f'{start + 30} {start + 70} RIGHT']
    elif args.game == 'canyon-racer':
        # Let the credits/title settle, enter the first-run help screen,
        # dismiss help, then issue jump holds during gameplay.
        events += ['900 905 START', '1500 1505 A']
        events += [f'{start} {start + 30} A' for start in range(1800, 3600, 90)]
    else:
        # Select set 5, whose default pieces are single blocks. Fill one
        # bottom row, then move the newly spawned piece after the clear.
        events += ['100 105 DOWN', '125 130 DOWN', '150 155 DOWN',
                   '175 180 DOWN', '210 215 A']
        target_offsets = (-4, -5, -3, -2, -1, 0, 1, 2, 3, 4)
        for piece, offset in enumerate(target_offsets):
            start = 300 + piece * 80
            events.append(f'{start} {start + 60} DOWN')
            button = 'LEFT' if offset < 0 else 'RIGHT'
            for tap in range(abs(offset)):
                press_frame = start + tap * 2
                events.append(f'{press_frame} {press_frame + 1} {button}')
        events += ['1110 1111 RIGHT', '1120 1121 RIGHT']
    (output / 'input.txt').write_text('\n'.join(events) + '\n')
    original_save_hash = sha256(args.save) if args.save else None
    def run_profile(input_path, frame_path, save_path):
        command = [str(args.runner.resolve()), str(args.core.resolve()),
                   str(args.rom.resolve()), '3600', str(frame_path),
                   str(save_path), str(input_path)]
        if args.save:
            command.append(str(args.save.resolve()))
        return command

    command = run_profile(output / 'input.txt', output / 'frame.rgb',
                          output / 'output.srm')
    # A successful exit must produce fresh artifacts, never reuse an old run.
    stale_outputs = ['frame.rgb', 'frame.ppm', 'output.srm']
    if args.game == 'big2small' and not args.save:
        stale_outputs += [
            'progress-sequence.rgb', 'empty-slot-frame.rgb', 'empty-slot.srm',
            'empty-title-frame.rgb', 'empty-title.srm',
            'empty-title-sequence.rgb', 'resume-title-frame.rgb',
            'resume-title.srm', 'resume-title-sequence.rgb',
            'resume-gameplay-frame.rgb', 'resume-gameplay.srm',
            'resume-gameplay-sequence.rgb', 'puzzle-completed.ppm',
            'empty-title.ppm', 'resume-title.ppm',
            'empty-slot-runner.log', 'empty-title-runner.log',
            'resume-title-runner.log', 'resume-gameplay-runner.log',
            'empty-slot-input.txt', 'neutral-input.txt',
            'resume-gameplay-input.txt',
            'resume-gameplay-before.ppm', 'resume-gameplay-after.ppm']
    if args.game == 'adjustris':
        baseline_events = ['100 105 DOWN', '125 130 DOWN',
                           '150 155 DOWN', '175 180 DOWN', '210 215 A']
        (output / 'baseline-input.txt').write_text(
            '\n'.join(baseline_events) + '\n')
        stale_outputs += ['baseline-frame.rgb', 'baseline-frame.ppm',
                          'baseline-output.srm', 'progress-sequence.rgb',
                          'baseline-progress-sequence.rgb',
                          'progress-full-row.ppm', 'progress-row-cleared.ppm',
                          'progress-next-piece-moved.ppm']
    for name in stale_outputs:
        (output / name).unlink(missing_ok=True)
    run_environment = os.environ.copy()
    for capture_variable in ('LOTUSGB_CAPTURE_SEQUENCE',
                             'LOTUSGB_CAPTURE_START', 'LOTUSGB_CAPTURE_END'):
        run_environment.pop(capture_variable, None)
    if args.game == 'adjustris':
        sequence_path = output / 'progress-sequence.rgb'
        run_environment.update({
            'LOTUSGB_CAPTURE_SEQUENCE': str(sequence_path),
            'LOTUSGB_CAPTURE_START': str(ADJUSTRIS_CAPTURE_START),
            'LOTUSGB_CAPTURE_END': str(ADJUSTRIS_CAPTURE_END),
        })
    elif args.game == 'big2small' and not args.save:
        sequence_path = output / 'progress-sequence.rgb'
        run_environment.update({
            'LOTUSGB_CAPTURE_SEQUENCE': str(sequence_path),
            'LOTUSGB_CAPTURE_START': str(BIG2SMALL_CAPTURE_START),
            'LOTUSGB_CAPTURE_END': str(BIG2SMALL_CAPTURE_END),
        })
    result = subprocess.run(command, capture_output=True, text=True,
                            env=run_environment)
    (output / 'runner.log').write_text(result.stdout + result.stderr)
    unchanged = not args.save or sha256(args.save) == original_save_hash
    scope = ('Callback delivery and scripted execution only; inspect frame.ppm manually. '
             'Not an audio-quality, level-completion or save-persistence assertion.')
    if args.game == 'adjustris':
        scope = ('Fills and clears one row with the default single-block set, then checks '
                 'that the next piece responds to right input. This is a bounded gameplay '
                 'regression, not full game compatibility or hardware-accuracy evidence.')
    elif args.game == 'big2small' and args.save:
        scope = ('Restores the supplied player save read-only and checks callback delivery '
                 'and scripted execution. The fresh-save/resume regression is skipped '
                 'when --save is supplied.')
    elif args.game == 'big2small':
        scope = ('Completes Big2Small Peanut Pathway, saves to a fresh test SRAM image, '
                 'restores it in independent runner processes, compares the 00% and 02% '
                 'save screens, and checks further directional input. This is runner/core '
                 'evidence, not a RetroArch disk-persistence or hardware test.')
    report = dict(game=args.game, rom_sha256=sha256(args.rom), core_sha256=sha256(args.core),
                  restored_save_sha256=original_save_hash, original_save_unchanged=unchanged,
                  runner_exit_code=result.returncode, command=command, scope=scope)
    validation_error = None
    if result.returncode == 0:
        try:
            rgb = (output / 'frame.rgb').read_bytes()
            save_ram = (output / 'output.srm').read_bytes()
            validate_run(result.stdout, rgb, save_ram)
            (output / 'frame.ppm').write_bytes(b'P6\n160 144\n255\n' + rgb)
            report['frame_sha256'] = hashlib.sha256(rgb).hexdigest()
            report['callbacks'] = result.stdout.strip()
            if args.game == 'adjustris':
                baseline_command = run_profile(
                    output / 'baseline-input.txt',
                    output / 'baseline-frame.rgb',
                    output / 'baseline-output.srm')
                baseline_sequence_path = (
                    output / 'baseline-progress-sequence.rgb')
                baseline_environment = os.environ.copy()
                baseline_environment.update({
                    'LOTUSGB_CAPTURE_SEQUENCE': str(baseline_sequence_path),
                    'LOTUSGB_CAPTURE_START': str(ADJUSTRIS_CAPTURE_START),
                    'LOTUSGB_CAPTURE_END': str(ADJUSTRIS_CAPTURE_END),
                })
                baseline = subprocess.run(
                    baseline_command, capture_output=True, text=True,
                    env=baseline_environment)
                (output / 'baseline-runner.log').write_text(
                    baseline.stdout + baseline.stderr)
                report['baseline_command'] = baseline_command
                report['baseline_runner_exit_code'] = baseline.returncode
                if baseline.returncode:
                    raise ValueError('baseline game-start run failed')
                baseline_rgb = (output / 'baseline-frame.rgb').read_bytes()
                baseline_ram = (output / 'baseline-output.srm').read_bytes()
                validate_run(baseline.stdout, baseline_rgb, baseline_ram)
                if baseline_ram != save_ram:
                    raise ValueError('gameplay inputs unexpectedly changed SRAM')
                try:
                    inspect_adjustris_progress(
                        baseline_sequence_path, ADJUSTRIS_CAPTURE_START,
                        ADJUSTRIS_CAPTURE_END)
                except ValueError as error:
                    if str(error) != 'script did not visibly fill one complete row':
                        raise ValueError(
                            'start-only baseline failed unexpectedly: ' + str(error))
                    report['baseline_progress_rejected'] = str(error)
                else:
                    raise ValueError(
                        'start-only baseline unexpectedly passed the progress check')
                (output / 'baseline-frame.ppm').write_bytes(
                    b'P6\n160 144\n255\n' + baseline_rgb)
                changed_pixels = differing_pixel_count(baseline_rgb, rgb)
                report['baseline_frame_sha256'] = hashlib.sha256(
                    baseline_rgb).hexdigest()
                report['gameplay_input_differing_pixels'] = changed_pixels
                report['gameplay_input_minimum_differing_pixels'] = 200
                if changed_pixels < 200:
                    raise ValueError(
                        'gameplay inputs did not visibly change the board state')
                progress = inspect_adjustris_progress(
                    output / 'progress-sequence.rgb',
                    ADJUSTRIS_CAPTURE_START, ADJUSTRIS_CAPTURE_END)
                report['progress_check'] = progress
                report['progress_capture_frames'] = {
                    'start_inclusive': ADJUSTRIS_CAPTURE_START,
                    'end_exclusive': ADJUSTRIS_CAPTURE_END}
                sequence = (output / 'progress-sequence.rgb').read_bytes()
                for artifact, key in (
                    ('progress-full-row.ppm', 'full_row_frame'),
                    ('progress-row-cleared.ppm', 'row_cleared_frame'),
                    ('progress-next-piece-moved.ppm',
                     'next_piece_control_after_frame')):
                    frame = progress[key]
                    offset = (frame - ADJUSTRIS_CAPTURE_START) * RGB_BYTES_PER_FRAME
                    rgb_frame = sequence[offset:offset + RGB_BYTES_PER_FRAME]
                    (output / artifact).write_bytes(
                        b'P6\n160 144\n255\n' + rgb_frame)
            elif args.game == 'big2small' and not args.save:
                report['save_cycle'] = inspect_big2small_save_cycle(
                    args, output, output / 'output.srm')
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
