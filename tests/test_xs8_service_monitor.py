"""Real service/monitor/decoder/verifier with synthetic GPIO, never hardware."""
import os
import pathlib
import socket
import subprocess
import tempfile
import time
import unittest

import xs8_gateway


class RealMonitorServiceTest(unittest.TestCase):
    def run_trace(self, trace, policy="strict", second=False):
        with tempfile.TemporaryDirectory(prefix="xs8-real-monitor-", dir="/tmp") as tmp:
            path = str(pathlib.Path(tmp) / "service.sock")
            log_path = pathlib.Path(tmp) / "service.log"
            env = dict(os.environ, XS8_GLOBAL_GAP_POLICY=policy,
                       XS8_MONITOR_TEST_TRACE=trace)
            with log_path.open("w") as log:
                process = subprocess.Popen([
                    ".build/xs8_service_monitor_test", "--socket", path,
                    "--allow-unrecovered-start"], env=env, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 3
                    while not pathlib.Path(path).exists():
                        self.assertIsNone(process.poll(), log_path.read_text())
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.005)
                    self.assertNotIn("FAKE_TX", log_path.read_text())

                    def request(command):
                        with socket.socket(socket.AF_UNIX) as client:
                            client.settimeout(2)
                            client.connect(path)
                            client.sendall((command + "\n").encode())
                            result = b""
                            while not result.endswith(b"\n"):
                                data = client.recv(16384)
                                self.assertTrue(data)
                                result += data
                            return result.decode().strip()

                    reply = request("SELECT_SOURCE CD")
                    status = request("STATUS")
                    parsed = xs8_gateway.read_status(path)
                    self.assertIn("global_gap_policy", parsed)
                    again = request("SELECT_SOURCE TUNER") if second else None
                    if second and again.startswith("OK"):
                        status = request("STATUS")
                finally:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=3)
            return reply, dict(field.split("=", 1) for field in status.split()[1:]), again, log_path.read_text()

    def test_one_valid_frame_and_warning_visibility(self):
        for policy, trace, count in (("strict", "valid", "0"), ("warn", "warn_valid", "1")):
            with self.subTest(policy=policy):
                reply, status, _, log = self.run_trace(trace, policy)
                self.assertIn("waveform=MATCH", reply)
                self.assertIn("receiver_execution=UNVERIFIED", reply)
                self.assertEqual(status["global_gap_policy"], policy)
                self.assertEqual(status["global_gap_warnings"], count)
                self.assertEqual(status["last_tx_global_gap_warnings"], count)
                self.assertEqual(status["monitor"], "HEALTHY")
                self.assertEqual(status["capture_integrity"], "WARN_GLOBAL_GAP" if count == "1" else "NO_KNOWN_GAPS")
                self.assertEqual(status["receiver_integrity"], "UNCERTAIN" if count == "1" else "HEALTHY")
                self.assertEqual(log.count("FAKE_TX"), 1)

    def test_ambiguous_or_malformed_window_never_matches(self):
        for policy in ("strict", "warn"):
            for trace in ("two", "malformed", "partial", "missing_tail"):
                with self.subTest(policy=policy, trace=trace):
                    reply, status, _, _ = self.run_trace(("warn_" if policy == "warn" else "") + trace, policy)
                    self.assertTrue(reply.startswith("ERR SOURCE_UNCERTAIN"), reply)
                    self.assertIn("waveform=INVALID", reply)
                    self.assertEqual(status["monitor"], "HEALTHY")

    def test_outside_traffic_is_not_a_tx_candidate(self):
        for trace in ("outside_valid", "outside_malformed", "outside_partial"):
            with self.subTest(trace=trace):
                reply, _, _, _ = self.run_trace(trace)
                self.assertIn("waveform=MATCH", reply)

    def test_post_tx_warning_is_cumulative_and_blocks_receiver_inference(self):
        reply, status, _, _ = self.run_trace("outside_warning", "warn")
        self.assertIn("waveform=MATCH", reply)
        self.assertEqual(status["global_gap_warnings"], "1")
        # Per-attempt counts conservatively include queued post-window traffic.
        self.assertEqual(status["last_tx_global_gap_warnings"], "1")
        self.assertEqual(status["receiver_integrity"], "UNCERTAIN")
        self.assertEqual(status["receiver_source"], "UNKNOWN")

    def test_warning_then_fatal_line_fault_blocks_next_tx(self):
        reply, status, again, log = self.run_trace("warn_line_fault", "warn", second=True)
        self.assertIn("waveform=NOT_OBSERVED", reply)
        self.assertEqual(status["monitor"], "FAILED")
        self.assertEqual(status["capture_integrity"], "FAILED")
        self.assertEqual(status["last_tx_capture_integrity"], "FAILED")
        self.assertEqual(status["global_gap_warnings"], "1")
        self.assertEqual(again, "ERR MONITOR_FAILED")
        self.assertEqual(log.count("FAKE_TX"), 1)

    def test_cumulative_warning_survives_clean_next_tx(self):
        _, status, again, log = self.run_trace("warn_once", "warn", second=True)
        self.assertIn("waveform=MATCH", again)
        self.assertEqual(status["global_gap_warnings"], "1")
        self.assertEqual(status["last_tx_global_gap_warnings"], "0")
        self.assertEqual(status["capture_integrity"], "WARN_GLOBAL_GAP")
        self.assertEqual(status["last_tx_capture_integrity"], "NO_KNOWN_GAPS")
        self.assertEqual(status["receiver_integrity"], "UNCERTAIN")
        self.assertEqual(log.count("FAKE_TX"), 2)

    def test_strict_still_fails_closed_on_global_only_gap(self):
        reply, status, _, _ = self.run_trace("warn_valid")
        self.assertIn("waveform=NOT_OBSERVED", reply)
        self.assertEqual(status["monitor"], "FAILED")


if __name__ == "__main__":
    unittest.main()
