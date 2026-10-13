import pathlib
import re
import socket
import subprocess
import tempfile
import time
import unittest


class ServiceSocketTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="xs8-socket-", dir="/tmp")
        self.path = str(pathlib.Path(self.temp.name) / "service.sock")
        self.log_path = pathlib.Path(self.temp.name) / "service.log"
        self.log = self.log_path.open("w")
        # This executable contains the production socket loop and a fake backend.
        # It does not link libgpiod or the production GPIO implementation.
        self.process = subprocess.Popen([
            str(pathlib.Path(".build/xs8_service_socket_test").resolve()),
            "--socket", self.path, "--allow-unrecovered-start",
            "--enable-research-tx", "--enable-research-chain",
            "--enable-research-context",
        ], stdout=subprocess.DEVNULL, stderr=self.log)
        deadline = time.monotonic() + 3
        while not pathlib.Path(self.path).exists():
            if self.process.poll() is not None or time.monotonic() >= deadline:
                self.fail("fake service did not open its socket")
            time.sleep(0.01)

    def tearDown(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()
        self.temp.cleanup()

    def connect(self):
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(1)
        client.connect(self.path)
        return client

    @staticmethod
    def command(client, text):
        client.sendall((text + "\n").encode("ascii"))
        response = bytearray()
        while not response.endswith(b"\n"):
            chunk = client.recv(1024)
            if not chunk:
                raise AssertionError("service closed before its reply")
            response.extend(chunk)
        return response.decode("ascii").strip()

    def request(self, text):
        with self.connect() as client:
            return self.command(client, text)

    def press(self, action):
        reply = self.request("CD_PRESS " + action)
        self.assertTrue(reply.startswith("OK CD_PRESS_TX"), reply)
        return re.search(r"hold_token=(\d+)", reply).group(1)

    def transmitted(self):
        return re.findall(r"^FAKE_TX (0x[0-9A-F]{2})$",
                          self.log_path.read_text(), re.M)

    def test_persistent_cec_and_short_lived_cd_status_clients(self):
        with self.connect() as cec:
            self.assertTrue(self.command(cec, "STATUS").startswith("OK state=IDLE"))
            token = self.press("TRACK_NEXT")
            # The PRESS socket has closed; CEC and STATUS stay connected/usable.
            self.assertIn("cd_state=HOLDING", self.request("STATUS"))
            self.assertEqual(self.command(cec, "VOLUME_PRESS UP"), "ERR CD_TRACK_BUSY")
            self.assertEqual(self.command(cec, "MUTE"), "ERR CD_TRACK_BUSY")
            for _ in range(5):
                time.sleep(0.45)
                self.assertEqual(self.request("CD_HEARTBEAT " + token),
                                 "OK CD_LEASE_RENEWED")
                self.assertIn("cd_state=HOLDING", self.request("STATUS"))
            self.assertEqual(self.transmitted(), ["0xFC"])
            self.assertTrue(self.request("CD_RELEASE " + token).startswith(
                "OK CD_RELEASE_TX"))
            self.assertEqual(self.command(cec, "VOLUME_PRESS UP"), "OK VOLUME_PRESS_TX")
            self.assertIn("state=HOLDING_UP", self.request("STATUS"))
            # Closing unrelated STATUS sockets cannot satisfy the CEC STOP obligation.
            self.assertEqual(self.transmitted(), ["0xFC", "0x72", "0x17", "0xF9"])
            self.assertEqual(self.command(cec, "VOLUME_RELEASE UP"), "OK VOLUME_RELEASE_TX")
            self.assertEqual(self.command(cec, "MUTE"), "OK MUTE_TX")

    def test_stale_tokens_from_separate_requests_cannot_touch_new_hold(self):
        first = self.press("TRACK_NEXT")
        self.assertTrue(self.request("CD_RELEASE " + first).startswith("OK CD_RELEASE_TX"))
        second = self.press("SEEK_FORWARD")
        self.assertNotEqual(first, second)
        self.assertEqual(self.request("CD_HEARTBEAT " + first), "ERR CD_NOT_OWNER")
        self.assertEqual(self.request("CD_RELEASE " + first), "ERR CD_NOT_OWNER")
        self.assertEqual(self.transmitted(), ["0xFC", "0x72", "0x7A"])
        self.assertTrue(self.request("CD_RELEASE " + second).startswith("OK CD_RELEASE_TX"))

    def test_disconnected_request_expires_lease_and_preserves_status(self):
        token = self.press("SEEK_BACKWARD")
        deadline = time.monotonic() + 3
        while "cd_lease_expired=1" not in self.request("STATUS"):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.05)
        self.assertEqual(self.transmitted(), ["0x6A", "0x72"])
        self.assertEqual(self.request("CD_HEARTBEAT " + token), "ERR CD_NOT_ACTIVE")
        status = self.request("STATUS")
        for field in ("state=IDLE", "cd_state=IDLE", "cd_release_owed=0",
                      "cd_last_result=LEASE_EXPIRED", "receiver_integrity=HEALTHY"):
            self.assertIn(field, status)
        self.assertEqual(self.request("CD_RELEASE " + token), "ERR CD_NOT_ACTIVE")
        fresh = self.press("TRACK_NEXT")
        self.assertNotEqual(token, fresh)
        self.assertIn("cd_lease_expired=0", self.request("STATUS"))
        self.assertEqual(self.request("CD_HEARTBEAT " + token), "ERR CD_NOT_OWNER")
        self.assertEqual(self.request("CD_RELEASE " + token), "ERR CD_NOT_OWNER")
        self.assertEqual(self.transmitted(), ["0x6A", "0x72", "0xFC"])
        self.assertTrue(self.request("CD_RELEASE " + fresh).startswith("OK CD_RELEASE_TX"))
        self.assertEqual(self.request("MUTE"), "OK MUTE_TX")

    def test_orderly_shutdown_releases_detached_hold(self):
        self.press("TRACK_PREVIOUS")
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=3), 0)
        self.assertEqual(self.transmitted(), ["0xEC", "0x72"])

    def test_volume_step_short_lived_client_and_cec_hold(self):
        idle = " state=IDLE stop_owed=0 context_may_be_open=0 receiver_execution=UNVERIFIED"
        self.assertEqual(self.request("VOLUME_STEP UP"),
                         "OK VOLUME_STEP_TX direction=UP" + idle)
        self.assertEqual(self.transmitted(), ["0x17", "0xF9", "0x09", "0x07"])
        with self.connect() as cec:
            self.assertEqual(self.command(cec, "VOLUME_PRESS DOWN"), "OK VOLUME_PRESS_TX")
            self.assertEqual(self.request("VOLUME_STEP DOWN"), "ERR VOLUME_BUSY")
            self.assertIn("state=HOLDING_DOWN", self.request("STATUS"))
            self.assertEqual(self.command(cec, "VOLUME_PRESS DOWN"), "OK VOLUME_PRESS_TX")
            self.assertEqual(self.command(cec, "VOLUME_RELEASE DOWN"), "OK VOLUME_RELEASE_TX")
        token = self.press("SEEK_FORWARD")
        self.assertEqual(self.request("VOLUME_STEP DOWN"), "ERR CD_TRACK_BUSY")
        self.assertTrue(self.request("CD_RELEASE " + token).startswith("OK CD_RELEASE_TX"))
        self.assertEqual(self.request("VOLUME_STEP DOWN"),
                         "OK VOLUME_STEP_TX direction=DOWN" + idle)
        self.assertEqual(self.transmitted(), [
            "0x17", "0xF9", "0x09", "0x07", "0x17", "0xE9", "0x09", "0x07",
            "0x7A", "0x72", "0x17", "0xE9", "0x09", "0x07"])
        self.assertIn("XS8 STOP reason=STEP result=TX_OK", self.log_path.read_text())

    def test_cd_transport_keys_single_frames_and_hold_exclusion(self):
        for line, action in (("CD_PLAY", "PLAY"), ("CD_PLAY_PAUSE", "PLAY_PAUSE"),
                             ("CD_STOP", "STOP")):
            self.assertEqual(self.request(line), "OK CD_TRANSPORT_TX action=" + action +
                             " gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED")
        self.assertEqual(self.transmitted(), ["0x41", "0xBC", "0x9C"])
        token = self.press("SEEK_BACKWARD")
        for line in ("CD_PLAY", "CD_PLAY_PAUSE", "CD_STOP"):
            self.assertEqual(self.request(line), "ERR CD_TRACK_BUSY")
        self.assertTrue(self.request("CD_RELEASE " + token).startswith("OK CD_RELEASE_TX"))
        self.assertEqual(self.request("CD_STOP").split()[:2], ["OK", "CD_TRANSPORT_TX"])
        self.assertEqual(self.transmitted(), ["0x41", "0xBC", "0x9C", "0x6A", "0x72", "0x9C"])
        status = self.request("STATUS")
        for field in ("cd_state=IDLE", "cd_action=STOP", "cd_last_result=TX_VERIFIED"):
            self.assertIn(field, status)

    def test_listener_accepts_bursts_beside_a_persistent_client(self):
        """Normal load: one long-lived owner plus bursts of short-lived requests
        connecting back to back. None may be refused before the service accepts."""
        with self.connect() as persistent:
            self.assertTrue(self.command(persistent, "STATUS").startswith("OK state="))
            for _ in range(5):
                burst = [self.connect() for _ in range(6)]  # No round trip between.
                try:
                    for client in burst:
                        self.assertTrue(self.command(client, "STATUS").startswith("OK state="))
                finally:
                    for client in burst:
                        client.close()
            for _ in range(40):
                self.assertTrue(self.request("STATUS").startswith("OK state="))
            self.assertTrue(self.command(persistent, "STATUS").startswith("OK state="))

    def test_client_limit_is_unchanged(self):
        clients = []
        try:
            for _ in range(8):  # kMaxClients
                clients.append(self.connect())
                self.assertTrue(self.command(clients[-1], "STATUS").startswith("OK state="))
            with self.connect() as extra:
                extra.settimeout(1)
                self.assertEqual(extra.recv(64), b"ERR BUSY\n")
                self.assertEqual(extra.recv(64), b"")
            self.assertTrue(self.command(clients[0], "STATUS").startswith("OK state="))
        finally:
            for client in clients:
                client.close()
        self.assertEqual(self.transmitted(), [])

    def test_volume_owner_disconnect_still_stops_with_other_clients(self):
        with self.connect() as cec:
            self.assertEqual(self.command(cec, "VOLUME_PRESS DOWN"),
                             "OK VOLUME_PRESS_TX")
            self.assertIn("state=HOLDING_DOWN", self.request("STATUS"))
            self.assertEqual(self.transmitted(), ["0x17", "0xE9"])
        deadline = time.monotonic() + 1
        while "state=IDLE " not in self.request("STATUS"):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        self.assertEqual(self.transmitted(), ["0x17", "0xE9", "0x09", "0x07"])

    def prepare_context(self):
        reply = self.request("RESEARCH_CONTEXT_PREPARE MUTE CONFIRM_NO_TX")
        self.assertTrue(reply.startswith("OK RESEARCH_CONTEXT_PREPARED"), reply)
        self.assertIn("context_entered=0 recovery_ready=0", reply)
        return re.search(r"token=(\d+)", reply).group(1)

    def test_attended_context_preparation_is_exclusive_passive_and_run_blocked(self):
        self.assertIn("CONFIRMATION_REQUIRED", self.request(
            "RESEARCH_CONTEXT_PREPARE MUTE NO"))
        self.assertIn("PROFILE_NOT_ALLOWLISTED", self.request(
            "RESEARCH_CONTEXT_PREPARE PHONO CONFIRM_NO_TX"))
        with self.connect() as cec:
            token = self.prepare_context()
            self.assertIn("state=RESEARCH_PREPARATION", self.request("STATUS"))
            for command in ("VOLUME_PRESS UP", "MUTE", "CD_TRACK_NEXT", "QUERY_STATUS",
                            "SELECT_SOURCE CD", "RESEARCH_SEND 0x17 100"):
                reply = self.command(cec, command)
                self.assertEqual(reply,
                                 "ERR RESEARCH_CONTEXT reason=EXCLUSIVE_PREPARATION")
            self.assertEqual(self.command(cec, "VOLUME_RELEASE UP"),
                             "OK VOLUME_RELEASE_TX")
            wrong = "1" if token != "1" else "2"
            self.assertIn("NOT_OWNER", self.request("RESEARCH_CONTEXT_CANCEL " + wrong))
            self.assertIn("NOT_OWNER", self.request("RESEARCH_CONTEXT_RUN " + wrong))
            self.assertIn("RECOVERY_EVIDENCE_MISSING", self.request(
                "RESEARCH_CONTEXT_RUN " + token))
            observed = self.request(f"RESEARCH_CONTEXT_OBSERVE {token} 100")
            experiment = re.search(r"id=(\d+)", observed).group(1)
            self.assertIn("tx_attempted=0", observed)
            deadline = time.monotonic() + 1
            while not self.request(f"RESEARCH_READ {experiment} 0").startswith(
                    "OK RESEARCH_DONE"):
                self.assertLess(time.monotonic(), deadline)
                self.request("RESEARCH_CONTEXT_HEARTBEAT " + token)
                time.sleep(0.02)
            status = self.request("RESEARCH_CONTEXT_STATUS")
            self.assertIn("panel_check=NOT_OBSERVED", status)
            self.assertNotIn("token=", status)
            self.assertEqual(self.transmitted(), [])
            self.assertIn("context_entered=0 tx_attempted=0", self.request(
                "RESEARCH_CONTEXT_CANCEL " + token))
            self.assertIn("state=IDLE", self.request("STATUS"))
            self.assertEqual(self.request("RESEARCH_SEND 0x17 100"),
                             "ERR RESEARCH reason=CONTEXT_ENTRY_FORBIDDEN")
            self.assertEqual(self.request("RESEARCH_CHAIN 2 200 100 0x17 0xD8"),
                             "ERR RESEARCH reason=CONTEXT_ENTRY_FORBIDDEN")
            self.assertEqual(self.transmitted(), [])

    def test_context_preparation_disappearance_and_shutdown_never_send_cleanup(self):
        token = self.prepare_context()
        deadline = time.monotonic() + 3
        while "state=EXPIRED" not in self.request("RESEARCH_CONTEXT_STATUS"):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.05)
        self.assertIn("NO_SESSION", self.request("RESEARCH_CONTEXT_RUN " + token))
        self.assertIn("state=IDLE", self.request("STATUS"))
        next_token = self.prepare_context()
        self.assertNotEqual(token, next_token)
        self.assertIn("NOT_OWNER", self.request("RESEARCH_CONTEXT_RUN " + token))
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=3), 0)
        self.assertEqual(self.transmitted(), [])

    def test_context_preparation_cannot_bypass_volume_or_cd_release_obligations(self):
        with self.connect() as cec:
            self.assertEqual(self.command(cec, "VOLUME_PRESS DOWN"), "OK VOLUME_PRESS_TX")
            self.assertIn("UNRESOLVED_CONTROL_STATE", self.request(
                "RESEARCH_CONTEXT_PREPARE MUTE CONFIRM_NO_TX"))
            self.assertIn("state=HOLDING_DOWN", self.request("STATUS"))
            self.assertEqual(self.transmitted(), ["0x17", "0xE9"])
            self.assertEqual(self.command(cec, "VOLUME_RELEASE DOWN"),
                             "OK VOLUME_RELEASE_TX")
        token = self.press("TRACK_NEXT")
        self.assertIn("UNRESOLVED_CONTROL_STATE", self.request(
            "RESEARCH_CONTEXT_PREPARE MUTE CONFIRM_NO_TX"))
        self.assertTrue(self.request("CD_RELEASE " + token).startswith("OK CD_RELEASE_TX"))
        self.assertEqual(self.transmitted(), ["0x17", "0xE9", "0x09", "0x07", "0xFC", "0x72"])


    # ---- Attended research CONTEXT session ----

    def enter_context(self, owner):
        reply = self.command(owner, "CONTEXT_SESSION_ENTER CONFIRM_ATTENDED 100")
        self.assertTrue(reply.startswith("OK CONTEXT_SESSION_ENTERED token="), reply)
        self.assertIn("byte=0x17 gpio_tx=TX_OK waveform=MATCH context_may_be_open=1", reply)
        self.assertRegex(reply, r" capture_id=\d+ frames=0x17:TX_OK:MATCH:\d+:\d+$")
        return re.search(r"token=(\d+)", reply).group(1), reply

    def wait_capture(self, reply):
        capture = re.search(r"capture_id=(\d+)", reply).group(1)
        deadline = time.monotonic() + 2
        while True:
            done = self.request(f"RESEARCH_READ {capture} 0")
            if done.startswith("OK RESEARCH_DONE"):
                return done
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)

    def wait_idle(self, seconds=1.0):
        deadline = time.monotonic() + seconds
        while not self.request("STATUS").startswith("OK state=IDLE "):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)

    def test_context_session_owner_send_exit_and_exclusivity(self):
        # Open the second connection only after the first is accepted: the
        # service listens with backlog 1 (tracked separately).
        with self.connect() as owner:
            token, entered = self.enter_context(owner)
            other = self.connect()
            self.addCleanup(other.close)
            self.wait_capture(entered)
            status = self.request("STATUS")
            self.assertIn("state=RESEARCH_CONTEXT ", status)
            self.assertIn("context_may_be_open=1", status)
            self.assertNotIn(token, self.request("CONTEXT_SESSION_STATUS"))
            for command in ("MUTE", "VOLUME_PRESS UP", "SELECT_SOURCE CD",
                            "CD_TRACK_NEXT", "CD_PRESS SEEK_FORWARD"):
                self.assertEqual(self.command(other, command), "ERR RESEARCH_CONTEXT_ACTIVE")
            self.assertEqual(self.command(other, "VOLUME_RELEASE UP"), "OK VOLUME_RELEASE_TX")
            self.assertEqual(self.request("QUERY_STATUS"), "ERR RESEARCH_CONTEXT")
            self.assertEqual(self.request("RESEARCH_SEND 0x84 100"),
                             "ERR RESEARCH reason=RESEARCH_CONTEXT")
            self.assertEqual(self.request("RESEARCH_SEND 0x17 100"),
                             "ERR RESEARCH reason=CONTEXT_ENTRY_FORBIDDEN")
            self.assertIn("UNRESOLVED_CONTROL_STATE", self.request(
                "RESEARCH_CONTEXT_PREPARE MUTE CONFIRM_NO_TX"))
            # The token is useless on any connection but the one that entered.
            self.assertEqual(self.command(other, f"CONTEXT_SESSION_SEND {token} 0x78 100"),
                             "ERR CONTEXT_SESSION reason=NOT_OWNER")
            self.assertEqual(self.command(other, f"CONTEXT_SESSION_EXIT {token} 100"),
                             "ERR CONTEXT_SESSION reason=NOT_OWNER")
            self.assertEqual(self.command(owner, f"CONTEXT_SESSION_SEND {token} 0x17 100"),
                             "ERR CONTEXT_SESSION reason=NESTED_ENTRY_FORBIDDEN")
            sent = self.command(owner, f"CONTEXT_SESSION_SEND {token} 0x78 100")
            self.assertRegex(sent, r"^OK CONTEXT_SESSION_SENT byte=0x78 gpio_tx=TX_OK "
                                   r"waveform=MATCH context_may_be_open=1 "
                                   r"receiver_execution=UNVERIFIED capture_id=\d+ "
                                   r"frames=0x78:TX_OK:MATCH:\d+:\d+$")
            self.assertIn("kind=OBSERVE", self.wait_capture(sent))
            self.assertEqual(self.command(owner, f"CONTEXT_SESSION_HEARTBEAT {token}"),
                             "OK CONTEXT_SESSION_LEASE_RENEWED")
            exited = self.command(owner, f"CONTEXT_SESSION_EXIT {token} 100")
            self.assertRegex(exited, r"^OK CONTEXT_SESSION_EXITED byte=0x07 root_attempts=1 "
                                     r"root_gpio_tx=TX_OK root_waveform=MATCH "
                                     r"context_may_be_open=0 state=IDLE "
                                     r"root_verified=NOT_OBSERVED .*frames=0x07:TX_OK:MATCH")
            self.wait_capture(exited)
            self.assertEqual(self.command(owner, f"CONTEXT_SESSION_SEND {token} 0x78"),
                             "ERR CONTEXT_SESSION reason=NOT_ACTIVE")
        self.assertEqual(self.transmitted(), ["0x17", "0x78", "0x07"])
        self.assertNotIn(token, self.log_path.read_text())
        self.assertEqual(self.request("MUTE"), "OK MUTE_TX")

    def test_context_owner_disconnect_exits_through_service(self):
        with self.connect() as owner:
            self.enter_context(owner)
            self.assertIn("state=RESEARCH_CONTEXT ", self.request("STATUS"))
        self.wait_idle()
        self.assertIn("context_may_be_open=0", self.request("STATUS"))
        self.assertEqual(self.transmitted(), ["0x17", "0x07"])
        self.assertIn("reason=OWNER_DISCONNECTED", self.log_path.read_text())

    def test_context_lease_expiry_exits_while_owner_stays_connected(self):
        with self.connect() as owner:
            token, _ = self.enter_context(owner)
            self.wait_idle(seconds=4.0)
            self.assertEqual(self.command(owner, f"CONTEXT_SESSION_HEARTBEAT {token}"),
                             "ERR CONTEXT_SESSION reason=NOT_ACTIVE")
        self.assertEqual(self.transmitted(), ["0x17", "0x07"])
        self.assertIn("reason=LEASE_EXPIRED", self.log_path.read_text())
        self.assertIn("last=LEASE_EXPIRED_ROOT_TX_OK", self.request("CONTEXT_SESSION_STATUS"))

    def test_context_orderly_shutdown_exits(self):
        with self.connect() as owner:
            self.enter_context(owner)
            self.process.terminate()
            self.assertEqual(self.process.wait(timeout=3), 0)
        self.assertEqual(self.transmitted(), ["0x17", "0x07"])

    def test_context_requires_both_research_flags(self):
        self.process.terminate()
        self.process.wait(timeout=3)
        pathlib.Path(self.path).unlink(missing_ok=True)
        self.process = subprocess.Popen([
            str(pathlib.Path(".build/xs8_service_socket_test").resolve()),
            "--socket", self.path, "--allow-unrecovered-start", "--enable-research-context",
        ], stdout=subprocess.DEVNULL, stderr=self.log)
        deadline = time.monotonic() + 3
        while not pathlib.Path(self.path).exists():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        self.assertEqual(self.request("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED"),
                         "ERR CONTEXT_SESSION reason=DISABLED")
        self.assertIn("research_context=DISABLED", self.log_path.read_text())
        self.assertEqual(self.transmitted(), [])

if __name__ == "__main__":
    unittest.main()
