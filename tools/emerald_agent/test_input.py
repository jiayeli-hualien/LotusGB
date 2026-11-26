"""Focused checks for platform dispatch and release on interrupted presses."""

import unittest
from unittest import mock

import input as game_input


class WindowsInputTests(unittest.TestCase):
    @mock.patch.object(game_input.time, "sleep")
    @mock.patch.object(game_input, "windows_key_event")
    @mock.patch.object(game_input.platform, "system", return_value="Windows")
    def test_windows_press_sends_down_then_up(self, _system, send, sleep):
        game_input.press("start", 120)
        self.assertEqual(send.call_args_list, [mock.call(0x0D, True), mock.call(0x0D, False)])
        sleep.assert_called_once_with(0.12)

    @mock.patch.object(game_input.time, "sleep", side_effect=InterruptedError)
    @mock.patch.object(game_input, "windows_key_event")
    @mock.patch.object(game_input.platform, "system", return_value="Windows")
    def test_interrupted_press_releases_key(self, _system, send, _sleep):
        with self.assertRaises(InterruptedError):
            game_input.press("left", 120)
        self.assertEqual(send.call_args_list, [mock.call(0x25, True), mock.call(0x25, False)])


if __name__ == "__main__":
    unittest.main()
