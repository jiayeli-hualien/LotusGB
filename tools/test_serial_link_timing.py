import unittest

from analyze_serial_link_timing import parse_log_lines


class SerialLinkTimingLogTest(unittest.TestCase):
    def test_pairs_guest_progress_with_wall_wait_and_reports_pending(self):
        lines = [
            "[LotusGB] Serial state (transfer-start): "
            "session_us=100000 sequence=1 cpu_tcycles=1000 PC=1234 SB=A5 SC=81",
            "[LotusGB] Serial state (after-byte-pair): "
            "session_us=110000 sequence=1 cpu_tcycles=42943 PC=1240 SB=3C SC=7F",
            "[LotusGB] Serial byte pair: session_us=110000 "
            "local_sequence=1 peer_sequence=1 local=0xA5 peer=0x3C wait_us=10000",
            "[LotusGB] Serial state (transfer-start): "
            "session_us=120000 sequence=2 cpu_tcycles=50000 PC=1250 SB=5A SC=81",
            "[LotusGB] Serial state (peer-byte-timeout): "
            "session_us=5120000 sequence=2 cpu_tcycles=21000000 PC=1255 SB=5A SC=81",
        ]

        analysis = parse_log_lines(lines, "Host")

        self.assertEqual(analysis.timeouts, 1)
        self.assertEqual(analysis.incomplete, [2])
        self.assertEqual(len(analysis.samples()), 1)
        sample = analysis.samples()[0]
        self.assertEqual(sample.sequence, 1)
        self.assertEqual(sample.wall_wait_us, 10_000)
        self.assertEqual(sample.logged_wait_us, 10_000)
        self.assertEqual(sample.guest_cycles, 41_943)
        self.assertAlmostEqual(sample.guest_wall_ratio(4_194_304), 1.0, places=5)

    def test_ignores_unrelated_and_incomplete_state_lines(self):
        analysis = parse_log_lines([
            "[LotusGB] Netpacket diagnostics: session_us=99 bytes_sent=1",
            "[LotusGB] Serial state (transfer-start): sequence=1 session_us=2",
        ], "Client")

        self.assertEqual(analysis.starts, {})
        self.assertEqual(analysis.samples(), [])


if __name__ == "__main__":
    unittest.main()
