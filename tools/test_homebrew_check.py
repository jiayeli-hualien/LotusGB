#!/usr/bin/env python3
"""ROM-free regression tests for the homebrew runner output contract."""
import unittest
import json
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

from measure_tobu_input import validate_tobudx_cgb_header
from run_homebrew_check import (differing_pixel_count, main,
                                validate_big2small_cgb_header,
                                validate_big2small_save_cycle,
                                validate_adjustris_observations, validate_run)


class HomebrewOutputTests(unittest.TestCase):
    summary = ('frames_requested=3600 video_frames=3600 audio_frames=2893139 '
               'save_ram_bytes=8192 width=160 height=144\n')
    rgb = bytes(160 * 144 * 3)
    ram = bytes(8192)

    def test_valid_output(self):
        validate_run(self.summary, self.rgb, self.ram)

    def test_invalid_callback_summaries(self):
        invalid = (
            '',
            self.summary + self.summary,
            self.summary.replace('video_frames=3600', 'video_frames=3599'),
            self.summary.replace('frames_requested=3600', 'frames_requested=1'),
            self.summary.replace('audio_frames=2893139', 'audio_frames=0'),
            self.summary.replace('width=160', 'width=144'),
            self.summary.replace('save_ram_bytes=8192', 'save_ram_bytes=0'),
        )
        for summary in invalid:
            with self.subTest(summary=summary), self.assertRaises(ValueError):
                validate_run(summary, self.rgb, self.ram)

    def test_invalid_artifact_sizes(self):
        for rgb, ram in ((self.rgb[:-1], self.ram),
                         (self.rgb + b'\0', self.ram),
                         (self.rgb, self.ram[:-1]),
                         (self.rgb, self.ram + b'\0')):
            with self.subTest(rgb=len(rgb), ram=len(ram)), self.assertRaises(ValueError):
                validate_run(self.summary, rgb, ram)

    def test_differing_pixel_count_compares_rgb_pixels(self):
        first = bytes(160 * 144 * 3)
        second = bytearray(first)
        second[0:3] = b'\xff\xff\xff'
        second[7 * 3:8 * 3] = b'\x01\x02\x03'
        self.assertEqual(differing_pixel_count(first, bytes(second)), 2)
        with self.assertRaises(ValueError):
            differing_pixel_count(first, first[:-1])

    def test_big2small_save_cycle_requires_progress_and_post_restore_input(self):
        empty_save = bytes(8192)
        completed_save = bytearray(empty_save)
        completed_save[0] = 1
        empty_title = bytearray(self.rgb)
        completed_title = bytearray(self.rgb)
        before_input = bytearray(self.rgb)
        after_input = bytearray(self.rgb)
        for pixel in range(10):
            completed_title[pixel * 3:pixel * 3 + 3] = b'\xff\xff\xff'
        for pixel in range(100):
            after_input[pixel * 3:pixel * 3 + 3] = b'\xff\xff\xff'

        result = validate_big2small_save_cycle(
            empty_save, bytes(completed_save), bytes(empty_title),
            bytes(completed_title), bytes(before_input), bytes(after_input))
        self.assertEqual(result['sram_bytes_changed_from_empty_slot'], 1)
        self.assertEqual(result['empty_vs_restored_title_differing_pixels'], 10)
        self.assertEqual(result['post_restore_input_differing_pixels'], 100)

    def test_big2small_save_cycle_rejects_missing_progress_or_input_response(self):
        title = bytearray(self.rgb)
        progressed_title = bytearray(self.rgb)
        for pixel in range(10):
            progressed_title[pixel * 3:pixel * 3 + 3] = b'\xff\xff\xff'
        moved = bytearray(self.rgb)
        for pixel in range(100):
            moved[pixel * 3:pixel * 3 + 3] = b'\xff\xff\xff'

        with self.assertRaisesRegex(ValueError, 'did not change cartridge SRAM'):
            validate_big2small_save_cycle(
                self.ram, self.ram, self.rgb, bytes(progressed_title),
                self.rgb, bytes(moved))
        with self.assertRaisesRegex(ValueError, 'did not visibly differ'):
            validate_big2small_save_cycle(
                self.ram, bytes([1]) + self.ram[1:], self.rgb, self.rgb,
                self.rgb, bytes(moved))
        with self.assertRaisesRegex(ValueError, 'did not visibly respond'):
            validate_big2small_save_cycle(
                self.ram, bytes([1]) + self.ram[1:], self.rgb,
                bytes(progressed_title), self.rgb, self.rgb)


    def test_adjustris_requires_row_clear_and_post_clear_control(self):
        full_row = {(17, column) for column in range(10)}
        observations = [
            (1000, {(2, 5)}),
            (1010, full_row),
            (1055, set()),
            (1056, {(2, 5)}),
            (1068, {(2, 7)}),
        ]
        result = validate_adjustris_observations(observations)
        self.assertEqual(result['full_row_frame'], 1010)
        self.assertEqual(result['row_cleared_frame'], 1055)
        self.assertEqual(result['next_piece_control_start_cell'], [2, 5])
        self.assertEqual(result['next_piece_control_end_cell'], [2, 7])

    def test_adjustris_rejects_incomplete_or_unresponsive_play(self):
        full_row = {(17, column) for column in range(10)}
        cases = (
            [(1000, set()), (1010, {(17, column) for column in range(9)})],
            [(1000, full_row), (1101, set())],
            [(1000, full_row), (1040, set()),
             (1041, {(2, 5)}), (1050, {(2, 5)})],
        )
        for observations in cases:
            with self.subTest(observations=observations), \
                 self.assertRaises(ValueError):
                validate_adjustris_observations(observations)

    def test_big2small_game_profile_requires_cgb_mbc5_battery_geometry(self):
        rom = bytearray(0x150)
        rom[0x143] = 0x80
        rom[0x147] = 0x1B
        rom[0x149] = 0x02
        self.assertEqual(validate_big2small_cgb_header(rom), {
            'cgb_header': '0x80',
            'mapper': 'MBC5 + RAM + battery',
            'save_ram_bytes': 8192,
        })
        for offset, value, message in (
                (0x143, 0x00, 'CGB-compatible'),
                (0x147, 0x13, 'MBC5'),
                (0x149, 0x03, '8-KiB')):
            invalid = bytearray(rom)
            invalid[offset] = value
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, message):
                validate_big2small_cgb_header(invalid)

    def test_tobudx_profile_requires_cgb_mbc1_battery_geometry(self):
        rom = bytearray(0x150)
        rom[0x143] = 0x80
        rom[0x147] = 0x03
        rom[0x149] = 0x02
        self.assertEqual(validate_tobudx_cgb_header(rom), {
            'cgb_header': '0x80',
            'mapper': 'MBC1 + RAM + battery',
            'save_ram_bytes': 8192,
        })
        for offset, value, message in (
                (0x143, 0x00, 'CGB-compatible'),
                (0x147, 0x1B, 'MBC1'),
                (0x149, 0x03, '8-KiB')):
            invalid = bytearray(rom)
            invalid[offset] = value
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, message):
                validate_tobudx_cgb_header(invalid)

    def test_success_exit_without_fresh_artifacts_is_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root / 'game.gb'
            synthetic_rom = bytearray(0x150)
            synthetic_rom[0x143] = 0x80
            synthetic_rom[0x147] = 0x1B
            synthetic_rom[0x149] = 0x02
            rom.write_bytes(synthetic_rom)
            output = root / 'output'
            output.mkdir()
            (output / 'frame.rgb').write_bytes(self.rgb)
            (output / 'output.srm').write_bytes(self.ram)
            (output / 'frame.ppm').write_bytes(b'stale preview')
            core = root / 'core.so'
            runner = root / 'runner'
            core.write_bytes(b'synthetic core')
            runner.write_bytes(b'synthetic runner')
            command = ['run_homebrew_check.py', '--rom', str(rom),
                       '--core', str(core), '--runner', str(runner),
                       '--output-dir', str(output)]
            digest = '59c096333f93c12c8eb25a5fcffa2be15001abcd5d20b9a82c2bc2dae7e625b2'
            completed = subprocess.CompletedProcess([], 0, self.summary, '')
            with patch('sys.argv', command), \
                 patch('run_homebrew_check.hashlib.sha256') as hash_function, \
                 patch('run_homebrew_check.subprocess.run', return_value=completed), \
                 patch('builtins.print'):
                # Synthetic harness data only; no downloaded ROM is required.
                hash_function.return_value.hexdigest.return_value = digest
                with self.assertRaises(SystemExit):
                    main()
            report = json.loads((output / 'report.json').read_text())
            self.assertEqual(report['runner_exit_code'], 0)
            self.assertIsNotNone(report['validation_error'])
            self.assertNotIn('frame_sha256', report)
            self.assertFalse((output / 'frame.ppm').exists())


if __name__ == '__main__':
    unittest.main()
