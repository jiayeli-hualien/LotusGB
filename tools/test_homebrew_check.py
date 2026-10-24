#!/usr/bin/env python3
"""ROM-free regression tests for the homebrew runner output contract."""
import unittest
import json
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

from run_homebrew_check import main, validate_run


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


    def test_success_exit_without_fresh_artifacts_is_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root / 'game.gb'
            rom.write_bytes(b'synthetic ROM')
            output = root / 'output'
            output.mkdir()
            (output / 'frame.rgb').write_bytes(self.rgb)
            (output / 'output.srm').write_bytes(self.ram)
            (output / 'frame.ppm').write_bytes(b'stale preview')
            command = ['run_homebrew_check.py', '--rom', str(rom),
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
