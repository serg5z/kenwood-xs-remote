"""Real service/monitor/decoder/verifier with synthetic GPIO, never hardware."""
from itertools import product
import os
import pathlib
import re
import socket
import subprocess
import tempfile
import time
import unittest

import xs8_gateway


class RealMonitorServiceTest(unittest.TestCase):
    def run_trace(self, trace, policy="strict", second=False, command="SELECT_SOURCE CD", snapshots=False):
        with tempfile.TemporaryDirectory(prefix="xs8-real-monitor-", dir="/tmp") as tmp:
            path = str(pathlib.Path(tmp) / "service.sock")
            log_path = pathlib.Path(tmp) / "service.log"
            env = dict(os.environ, XS8_GLOBAL_GAP_POLICY=policy,
                       XS8_MONITOR_TEST_TRACE=trace,
                       XS8_FAILURE_SNAPSHOTS="1" if snapshots else "0")
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

                    reply = request(command)
                    status = request("STATUS")
                    parsed = xs8_gateway.read_status(path)
                    self.assertIn("global_gap_policy", parsed)
                    again = request(second if isinstance(second, str) else
                                    "SELECT_SOURCE TUNER") if second else None
                    if second and again.startswith("OK"):
                        status = request("STATUS")
                finally:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=3)
            return reply, dict(field.split("=", 1) for field in status.split()[1:]), again, log_path.read_text()

    def test_invalid_stop_snapshot_does_not_change_retry_or_latched_fault(self):
        for enabled in (False, True):
            with self.subTest(snapshots=enabled):
                reply, status, again, log = self.run_trace(
                    "volume_low_stop", command="VOLUME_STEP DOWN",
                    second="VOLUME_STEP UP", snapshots=enabled)
                self.assertIn("state=FAULT_LATCHED stop_owed=0 context_may_be_open=0", reply)
                self.assertTrue(again.startswith("ERR"))
                self.assertEqual(status["monitor"], "HEALTHY")
                self.assertEqual(status["state"], "FAULT_LATCHED")
                self.assertEqual(status["stop_owed"], "0")
                self.assertEqual(status["last_stop_waveform"], "MATCH")
                self.assertEqual(status["global_gap_warnings"], "0")
                self.assertEqual(re.findall(r"FAKE_TX (0x[0-9A-F]{2})", log),
                                 ["0x17", "0xE9", "0x09", "0x09", "0x07"])
                self.assertIn("gpio_tx=TX_OK waveform=INVALID", log)
                self.assertIn("reason=LOW_TIMING", log)
                self.assertIn("XS8 STOP reason=STEP result=TX_OK retries=1", log)
                if enabled:
                    self.assertIn("rejection=DECODE_INVALID", log)
                    self.assertIn("XS8_RAW,", log)
                    self.assertGreater(log.index("XS8_FAILURE_BEGIN"),
                                       log.index("XS8 ROOT result=TX_OK"))
                    self.assertGreater(log.index("XS8_FAILURE_BEGIN"),
                                       log.index("state=FAULT_LATCHED"))
                else:
                    self.assertNotIn("XS8_FAILURE_BEGIN", log)

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

    def test_mute_transaction_and_warning_confidence(self):
        for trace, policy, warnings in (("valid", "strict", 0),
                                        ("warn_once", "warn", 1),
                                        ("warn_valid", "warn", 3)):
            with self.subTest(trace=trace, policy=policy):
                reply, status, _, log = self.run_trace(trace, policy, command="MUTE")
                self.assertEqual(reply, "OK MUTE_TX")
                self.assertEqual(re.findall(r"FAKE_TX (0x[0-9A-F]{2})", log),
                                 ["0x17", "0xD8", "0x07"])
                for key, value in {"state": "IDLE", "monitor": "HEALTHY",
                                   "stop_owed": "0", "context_may_be_open": "0",
                                   "last_byte": "0x07", "last_gpio_tx": "TX_OK",
                                   "last_waveform": "MATCH",
                                   "global_gap_warnings": str(warnings),
                                   "capture_integrity": "WARN_GLOBAL_GAP" if warnings else "NO_KNOWN_GAPS",
                                   "receiver_integrity": "UNCERTAIN" if warnings else "HEALTHY",
                                   "receiver_execution": "UNVERIFIED"}.items():
                    self.assertEqual(status[key], value, key)
                self.assertIn("XS8 ROOT result=TX_OK retries=0", log)
                self.assertNotIn("state=FAULT_LATCHED", log)

    def test_volume_start_and_stop_global_only_warnings(self):
        for (direction, start), (trace, warnings) in product(
                (("DOWN", "E9"), ("UP", "F9")),
                (("volume_control", 0), ("volume_warn_start", 1),
                 ("volume_warn_stop", 1), ("volume_warn_both", 2))):
            with self.subTest(direction=direction, trace=trace):
                reply, status, _, log = self.run_trace(
                    trace, "warn", command=f"VOLUME_STEP {direction}")
                self.assertEqual(reply, f"OK VOLUME_STEP_TX direction={direction} state=IDLE "
                                 "stop_owed=0 context_may_be_open=0 "
                                 "receiver_execution=UNVERIFIED")
                self.assertEqual(re.findall(r"FAKE_TX (0x[0-9A-F]{2})", log),
                                 ["0x17", f"0x{start}", "0x09", "0x07"])
                self.assertIn(f"XS8 START direction={direction} result=TX_OK", log)
                self.assertIn("XS8 STOP evidence gpio_tx=TX_OK waveform=MATCH", log)
                self.assertIn("XS8 STOP reason=STEP result=TX_OK retries=0", log)
                self.assertIn("XS8 ROOT result=TX_OK retries=0", log)
                self.assertNotIn("state=FAULT_LATCHED", log)
                for key, value in {"state": "IDLE", "stop_owed": "0",
                                   "context_may_be_open": "0", "monitor": "HEALTHY",
                                   "last_byte": "0x07", "last_waveform": "MATCH",
                                   "last_stop_waveform": "MATCH",
                                   "global_gap_warnings": str(warnings),
                                   "last_tx_global_gap_warnings": "0",
                                   "last_tx_capture_integrity": "NO_KNOWN_GAPS",
                                   "receiver_execution": "UNVERIFIED"}.items():
                    self.assertEqual(status[key], value, key)
                self.assertEqual(status["capture_integrity"],
                                 "WARN_GLOBAL_GAP" if warnings else "NO_KNOWN_GAPS")
                self.assertEqual(status["receiver_integrity"],
                                 "UNCERTAIN" if warnings else "HEALTHY")
                self.assertEqual(log.count("WARNING reason=GLOBAL_ONLY_SEQUENCE_GAP"), warnings)
                for byte, warned in ((start, "start" in trace or "both" in trace),
                                     ("09", "stop" in trace or "both" in trace)):
                    self.assertIn(f"FAKE_TX 0x{byte} trace=" +
                                  ("warn_valid" if warned else "valid"), log)

    def test_fatal_capture_fault_during_stop_after_verified_volume_start(self):
        # Entry and START match (START has an exempt warning). A per-line fault
        # in STOP fails the real monitor. Future GPIO sends still complete, but
        # cannot be waveform-verified; STOP debt must survive, and ROOT is owed.
        for (direction, start), command in product(
                (("DOWN", "E9"), ("UP", "F9")),
                ("VOLUME_STEP DOWN", "VOLUME_STEP UP", "MUTE", "SELECT_SOURCE TUNER")):
            with self.subTest(direction=direction, subsequent=command):
                reply, status, again, log = self.run_trace(
                    "volume_fatal_stop", "warn", second=command,
                    command=f"VOLUME_STEP {direction}")
                self.assertEqual(reply, f"ERR VOLUME_STEP_UNCERTAIN direction={direction} "
                                 "state=FAULT_LATCHED stop_owed=1 context_may_be_open=1 "
                                 "receiver_execution=UNVERIFIED")
                self.assertEqual(again, "ERR MONITOR_FAILED")
                for key, value in {"state": "FAULT_LATCHED", "monitor": "FAILED",
                                   "stop_owed": "1", "context_may_be_open": "1",
                                   "last_stop_waveform": "NOT_OBSERVED",
                                   "last_gpio_tx": "TX_OK",
                                   "capture_integrity": "FAILED",
                                   "last_tx_capture_integrity": "FAILED",
                                   "global_gap_warnings": "1",
                                   "receiver_integrity": "UNCERTAIN",
                                   "receiver_execution": "UNVERIFIED"}.items():
                    self.assertEqual(status[key], value, key)
                self.assertIn(f"XS8 START direction={direction} result=TX_OK", log)
                self.assertIn("monitor FAILED reason=LINE_SEQUENCE_GAP", log)
                self.assertIn("XS8 STOP reason=STEP result=TX_FAIL retries=2", log)
                self.assertIn("XS8 STOP evidence gpio_tx=TX_OK waveform=NOT_OBSERVED", log)
                # Three STOPs in step cleanup, three in the one-shot monitor
                # fault handler, then three at final orderly shutdown. STATUS
                # and the rejected request must not create an unbounded retry.
                self.assertEqual(re.findall(r"FAKE_TX (0x[0-9A-F]{2})", log),
                                 ["0x17", f"0x{start}"] + ["0x09"] * 9)
                self.assertNotIn("XS8 ROOT result=", log)


if __name__ == "__main__":
    unittest.main()
