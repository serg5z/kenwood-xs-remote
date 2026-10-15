import os
import pathlib
import re
import socket
import subprocess
import tempfile
import time
import unittest


class ServiceSocketTest(unittest.TestCase):
    extra_args = ()

    def environment(self):
        return None

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
            "--enable-research-context", *self.extra_args,
        ], stdout=subprocess.DEVNULL, stderr=self.log, env=self.environment())
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
        self.assertEqual(self.transmitted(), ["0x41", "0xBC", "0xA1"])
        token = self.press("SEEK_BACKWARD")
        for line in ("CD_PLAY", "CD_PLAY_PAUSE", "CD_STOP"):
            self.assertEqual(self.request(line), "ERR CD_TRACK_BUSY")
        self.assertTrue(self.request("CD_RELEASE " + token).startswith("OK CD_RELEASE_TX"))
        self.assertEqual(self.request("CD_STOP").split()[:2], ["OK", "CD_TRANSPORT_TX"])
        self.assertEqual(self.transmitted(), ["0x41", "0xBC", "0xA1", "0x6A", "0x72", "0xA1"])
        status = self.request("STATUS")
        for field in ("cd_state=IDLE", "cd_action=STOP", "cd_last_result=TX_VERIFIED"):
            self.assertIn(field, status)

    def wait_status(self, field, timeout=3):
        deadline = time.monotonic() + timeout
        while field not in self.request("STATUS"):
            self.assertLess(time.monotonic(), deadline, field)
            time.sleep(0.02)

    def test_key_hold_is_owned_by_its_connection(self):
        ok_up = " gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED"
        # One round trip per connection before opening the next: the service's
        # listen backlog of 1 can refuse back-to-back connects (known follow-up).
        with self.connect() as owner:
            self.command(owner, "STATUS")
            cec = self.connect()
            self.addCleanup(cec.close)
            self.assertEqual(self.command(owner, "KEY_DOWN SEEK_FORWARD"),
                             "OK KEY_DOWN_TX action=SEEK_FORWARD" + ok_up)
            for _ in range(4):
                time.sleep(0.3)
                self.assertEqual(self.command(owner, "KEY_DOWN SEEK_FORWARD"),
                                 "OK KEY_ALIVE action=SEEK_FORWARD")
            self.assertEqual(self.transmitted(), ["0x7A"])
            # Other connections, including short-lived ones, cannot end it.
            self.assertEqual(self.request("KEY_UP SEEK_FORWARD"), "ERR KEY_NOT_OWNER")
            self.assertEqual(self.command(cec, "VOLUME_PRESS UP"), "ERR CD_TRACK_BUSY")
            self.assertEqual(self.command(cec, "KEY_UP SEEK_FORWARD"), "ERR KEY_NOT_OWNER")
            self.assertIn("cd_state=HOLDING cd_action=SEEK_FORWARD", self.request("STATUS"))
            self.assertEqual(self.command(owner, "KEY_UP SEEK_FORWARD"),
                             "OK KEY_UP_TX action=SEEK_FORWARD" + ok_up)
            self.assertEqual(self.command(owner, "KEY_UP SEEK_FORWARD"),
                             "OK KEY_UP_NONE last=CLIENT_RELEASE")
        self.assertEqual(self.transmitted(), ["0x7A", "0x72"])

    def test_closing_the_owner_connection_releases_immediately(self):
        with self.connect() as other:
            self.command(other, "STATUS")  # Accepted before the next connect.
            with self.connect() as owner:
                self.assertTrue(self.command(owner, "KEY_DOWN SEEK_BACKWARD").startswith(
                    "OK KEY_DOWN_TX"))
            closed = time.monotonic()
            self.wait_status("cd_last_result=CLIENT_DISCONNECT")
            # Well inside the 1200 ms inactivity limit: the event loop released it.
            self.assertLess(time.monotonic() - closed, 0.5)
            self.assertEqual(self.transmitted(), ["0x6A", "0x72"])
            self.assertEqual(self.command(other, "STATUS").split()[1], "state=IDLE")

    def test_silent_open_owner_is_released_by_inactivity_and_cannot_restart(self):
        with self.connect() as owner:
            self.assertTrue(self.command(owner, "KEY_DOWN SEEK_FORWARD").startswith(
                "OK KEY_DOWN_TX"))
            pressed = time.monotonic()
            self.wait_status("cd_last_result=INACTIVITY")
            self.assertGreaterEqual(time.monotonic() - pressed, 1.1)
            self.assertEqual(self.command(owner, "KEY_DOWN SEEK_FORWARD"),
                             "ERR KEY_EXPIRED last=INACTIVITY")
            self.assertEqual(self.transmitted(), ["0x7A", "0x72"])
            self.assertEqual(self.command(owner, "KEY_UP SEEK_FORWARD"),
                             "OK KEY_UP_NONE last=INACTIVITY")
            self.assertTrue(self.command(owner, "KEY_DOWN SEEK_FORWARD").startswith(
                "OK KEY_DOWN_TX"))
        self.wait_status("cd_last_result=CLIENT_DISCONNECT")
        self.assertEqual(self.transmitted(), ["0x7A", "0x72", "0x7A", "0x72"])

    def test_token_hold_unchanged_and_not_releasable_by_key_up(self):
        token = self.press("SEEK_FORWARD")
        self.assertEqual(self.request("KEY_UP SEEK_FORWARD"), "ERR KEY_NOT_OWNER")
        self.assertEqual(self.request("KEY_DOWN SEEK_FORWARD"), "ERR KEY_BUSY")
        time.sleep(1.4)  # Past the KEY inactivity limit, inside the 2 s token lease.
        self.assertEqual(self.request("CD_HEARTBEAT " + token), "OK CD_LEASE_RENEWED")
        self.assertTrue(self.request("CD_RELEASE " + token).startswith("OK CD_RELEASE_TX"))
        self.assertEqual(self.transmitted(), ["0x7A", "0x72"])

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

    def test_exclusive_modes_cannot_start_during_key_hold_so_key_up_is_never_gated(self):
        """KEY_UP is not on the exclusive-mode allowlists. That is safe because
        none of those modes can begin while a CD hold owes its release, and none
        lets a hold begin while it is active."""
        with self.connect() as owner:
            self.assertTrue(self.command(owner, "KEY_DOWN SEEK_FORWARD").startswith(
                "OK KEY_DOWN_TX"))
            for line, refusal in (
                    ("QUERY_STATUS", "ERR CD_TRACK_BUSY"),
                    ("RESEARCH_CONTEXT_PREPARE MUTE CONFIRM_NO_TX",
                     "ERR RESEARCH_CONTEXT reason=UNRESOLVED_CONTROL_STATE"),
                    ("RESEARCH_SEND 0x84 100", "ERR RESEARCH reason=CD_TRACK_BUSY"),
                    ("RESEARCH_CHAIN 2 200 100 0x84 0x82", "ERR RESEARCH reason=CD_TRACK_BUSY"),
                    ("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED",
                     "ERR CONTEXT_SESSION reason=CD_BUSY")):
                self.assertEqual(self.request(line), refusal)
                self.assertIn("cd_state=HOLDING", self.request("STATUS"))
            self.assertEqual(self.request("KEY_UP SEEK_FORWARD"), "ERR KEY_NOT_OWNER")
            self.assertEqual(self.transmitted(), ["0x7A"])
            self.assertTrue(self.command(owner, "KEY_UP SEEK_FORWARD").startswith(
                "OK KEY_UP_TX action=SEEK_FORWARD"))
        self.assertEqual(self.transmitted(), ["0x7A", "0x72"])

    def test_orderly_shutdown_releases_connection_owned_hold(self):
        owner = self.connect()
        self.addCleanup(owner.close)
        self.assertTrue(self.command(owner, "KEY_DOWN SEEK_BACKWARD").startswith(
            "OK KEY_DOWN_TX"))
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=3), 0)
        self.assertEqual(self.transmitted(), ["0x6A", "0x72"])

    def test_context_keys_and_power_frames(self):
        ok = " gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED"
        self.assertEqual(self.request("SELECT_SOURCE VIDEO1"), "OK SOURCE_TX" + ok)
        self.assertEqual(self.request("SELECT_SOURCE VIDEO2"), "OK SOURCE_TX" + ok)
        self.assertEqual(self.request("TAPE2_MONITOR_TOGGLE"), "OK TAPE2_MONITOR_TX" + ok)
        self.assertEqual(self.request("POWER_ON"), "OK POWER_TX action=ON" + ok)
        self.assertEqual(self.request("POWER_OFF"), "OK POWER_TX action=OFF" + ok)
        self.assertEqual(self.transmitted(), [
            "0x17", "0x78", "0x07", "0x17", "0x88", "0x07", "0x17", "0x48", "0x07",
            "0x52", "0x62"])
        # TV1/TV2 remain compatibility aliases with the same wire bytes.
        self.assertEqual(self.request("SELECT_SOURCE TV1"), "OK SOURCE_TX" + ok)
        self.assertEqual(self.request("SELECT_SOURCE TV2"), "OK SOURCE_TX" + ok)
        self.assertEqual(self.transmitted()[-6:], ["0x17", "0x78", "0x07", "0x17", "0x88", "0x07"])
        self.assertIn("state=IDLE stop_owed=0 context_may_be_open=0", self.request("STATUS"))

    def test_web_volume_hold_released_by_closing_its_connection(self):
        with self.connect() as cec:
            self.command(cec, "STATUS")  # Accepted before the next connect.
            with self.connect() as owner:
                self.assertEqual(self.command(owner, "KEY_DOWN VOLUME_DOWN"),
                                 "OK KEY_DOWN_TX action=VOLUME_DOWN gpio_tx=TX_OK "
                                 "waveform=MATCH receiver_execution=UNVERIFIED")
                for _ in range(3):
                    time.sleep(0.3)
                    self.assertEqual(self.command(owner, "KEY_DOWN VOLUME_DOWN"),
                                     "OK KEY_ALIVE action=VOLUME_DOWN")
                self.assertEqual(self.command(cec, "VOLUME_PRESS DOWN"), "ERR VOLUME_BUSY")
                self.assertEqual(self.command(cec, "VOLUME_RELEASE DOWN"), "ERR VOLUME_NOT_OWNER")
                self.assertEqual(self.command(cec, "MUTE"), "ERR VOLUME_BUSY")
                self.assertEqual(self.request("POWER_OFF"), "ERR HOLDING_DOWN")
                self.assertEqual(self.transmitted(), ["0x17", "0xE9"])
            closed = time.monotonic()
            self.wait_status("state=IDLE stop_owed=0 context_may_be_open=0")
            self.assertLess(time.monotonic() - closed, 0.5)
            self.assertEqual(self.transmitted(), ["0x17", "0xE9", "0x09", "0x07"])
            self.assertIn("XS8 STOP reason=CLIENT_DISCONNECT", self.log_path.read_text())
            # CEC volume works normally afterwards.
            self.assertEqual(self.command(cec, "VOLUME_PRESS UP"), "OK VOLUME_PRESS_TX")
            self.assertEqual(self.command(cec, "VOLUME_RELEASE UP"), "OK VOLUME_RELEASE_TX")

    def test_max_hold_option_accepts_20000_and_nothing_above(self):
        binary = str(pathlib.Path(".build/xs8_service_socket_test").resolve())
        for value in ("20001", "99", "3000x"):
            result = subprocess.run([binary, "--socket", self.path + value,
                                     "--max-hold-ms", value],
                                    capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 2, value)
            self.assertIn("Invalid max hold (100..20000 ms)", result.stderr)
        path = self.path + ".20000"
        process = subprocess.Popen([binary, "--socket", path, "--allow-unrecovered-start",
                                    "--max-hold-ms", "20000"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 3
            while not pathlib.Path(path).exists():
                self.assertIsNone(process.poll(), "--max-hold-ms 20000 must be accepted")
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
        finally:
            process.terminate()
            process.wait(timeout=3)

    def test_default_max_hold_keeps_cec_and_web_holds_past_the_old_limit(self):
        # The default is the 20 s MAX_HOLD: keepalives every 400 ms (inside the
        # 1200 ms inactivity limit) hold well past the former 3 s cap.
        with self.connect() as owner:
            self.assertTrue(self.command(owner, "KEY_DOWN VOLUME_UP").startswith(
                "OK KEY_DOWN_TX action=VOLUME_UP"))
            started = time.monotonic()
            while time.monotonic() - started < 3.4:
                time.sleep(0.4)
                self.assertEqual(self.command(owner, "KEY_DOWN VOLUME_UP"),
                                 "OK KEY_ALIVE action=VOLUME_UP")
            self.assertIn("state=HOLDING_UP", self.request("STATUS"))
            self.assertTrue(self.command(owner, "KEY_UP VOLUME_UP").startswith(
                "OK KEY_UP_TX action=VOLUME_UP"))
        with self.connect() as cec:
            self.assertEqual(self.command(cec, "VOLUME_PRESS DOWN"), "OK VOLUME_PRESS_TX")
            started = time.monotonic()
            while time.monotonic() - started < 3.4:
                time.sleep(0.4)
                self.assertEqual(self.command(cec, "VOLUME_PRESS DOWN"), "OK VOLUME_PRESS_TX")
            self.assertIn("state=HOLDING_DOWN", self.request("STATUS"))
            self.assertEqual(self.command(cec, "VOLUME_RELEASE DOWN"), "OK VOLUME_RELEASE_TX")
        self.assertEqual(self.transmitted(), ["0x17", "0xF9", "0x09", "0x07",
                                              "0x17", "0xE9", "0x09", "0x07"])
        self.assertNotIn("MAX_HOLD", self.log_path.read_text())

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

    def test_cd_probe_is_disabled_by_default(self):
        self.assertEqual(self.request("PROBE_CD_CHANGER"), "ERR CD_PROBE reason=DISABLED")
        self.assertEqual(self.transmitted(), [])
        # STATUS is unchanged without the flag, so current STATUS parsers keep working.
        self.assertNotIn("cd_changer", self.request("STATUS"))
        self.assertIn("cd_presence_probe=DISABLED", self.log_path.read_text())


class ScriptedRxSocketTest(ServiceSocketTest):
    """Simulated RX timed from each fake TX (XS8_FAKE_RX_SCRIPT)."""

    for _name in [n for n in dir(ServiceSocketTest) if n.startswith("test_")]:
        locals()[_name] = None
    del _name

    def environment(self):
        self.script = pathlib.Path(tempfile.mkdtemp(prefix="xs8-rx-", dir="/tmp")) / "rx"
        self.script.write_text("")
        self.addCleanup(lambda: self.script.unlink(missing_ok=True))
        return dict(os.environ, XS8_FAKE_RX_SCRIPT=str(self.script))

    def rx(self, *lines):
        self.script.write_text("\n".join(lines) + "\n")

    def status(self):
        return dict(part.split("=", 1) for part in self.request("STATUS").split()[1:])

    def settle(self):
        """QUERY_STATUS SETTLE may wait up to 1.5 s before its 0.5 s window."""
        with self.connect() as client:
            client.sendall(b"QUERY_STATUS SETTLE\n")
            return self.read_reply(client)

    @staticmethod
    def read_reply(client, timeout=3):
        client.settimeout(timeout)
        reply = bytearray()
        while not reply.endswith(b"\n"):
            chunk = client.recv(1024)
            if not chunk:
                raise AssertionError("service closed before its reply")
            reply.extend(chunk)
        return reply.decode("ascii").strip()


# A recognised ROOT 0x13 reply: CD (legacy 0x1D), then MONITOR ON (legacy 0x14).
QUERY_CD_ON = "0x13 echo +3000:0x47/0x1D +6000:0xD7/0x14"
QUERY_TUNER_ON = "0x13 echo +3000:0x27/0x1B +6000:0xD7/0x14"


class QueryStatusSocketTest(ScriptedRxSocketTest):
    """QUERY_STATUS outcomes, QUERY_STATUS SETTLE and cache clearing after local TX."""

    def test_no_reply_is_distinct_from_partial_and_faults(self):
        for rx, reason in (("0x13 echo", "NO_REPLY"),
                           ("0x13 echo +3000:0x47/0x1D", "PARTIAL"),
                           ("0x13 echo +3000:INVALID", "INCOMPLETE"),
                           ("0x13 echo +3000:0x47/0x1D +4000:GAP", "EVENT_GAP"),
                           ("0x13", "TX_MISSING")):
            with self.subTest(reason=reason):
                self.rx(rx)
                reply = self.request("QUERY_STATUS")
                self.assertTrue(reply.startswith(f"ERR QUERY_STATUS reason={reason} "), reply)
                self.assertTrue(reply.endswith("receiver_execution=UNVERIFIED"), reply)
                self.assertIn("source_inferred=UNKNOWN", reply)
        self.assertEqual(self.status()["receiver_source"], "UNKNOWN")
        self.rx(QUERY_CD_ON)
        reply = self.request("QUERY_STATUS")
        self.assertTrue(reply.startswith("OK QUERY_STATUS reason=NONE "), reply)
        self.assertIn("source_inferred=CD monitor_inferred=ON", reply)
        status = self.status()
        self.assertEqual((status["receiver_source"], status["receiver_source_evidence"],
                          status["receiver_monitor"]), ("CD", "QUERY", "ON"))
        self.assertEqual(self.transmitted(), ["0x13"] * 6, "one frame each, never retried")

    def test_settle_on_a_quiet_bus_transmits_promptly(self):
        self.rx(QUERY_CD_ON)
        started = time.monotonic()
        reply = self.settle()
        self.assertTrue(reply.startswith("OK QUERY_STATUS reason=NONE "), reply)
        self.assertLess(time.monotonic() - started, 0.9)
        self.assertEqual(self.transmitted(), ["0x13"])
        self.assertRegex(self.log_path.read_text(), r"XS8 QUERY_STATUS settle waited_ms=\d+\n")

    def test_delayed_cd_stop_report_collides_without_settle(self):
        # The changer's 0x94 ~320 ms after STOP lands inside a 500 ms query window.
        self.rx("0xA1 echo +320000:0x94", QUERY_CD_ON)
        self.assertTrue(self.request("CD_STOP").startswith("OK CD_TRANSPORT_TX"))
        reply = self.request("QUERY_STATUS")
        self.assertTrue(reply.startswith("ERR QUERY_STATUS reason=EXTRA "), reply)
        self.assertRegex(reply, r" rx3=0x94/")
        self.assertEqual(self.status()["receiver_source"], "UNKNOWN")
        # With SETTLE the one query frame waits until the report has passed.
        time.sleep(1.05)
        self.assertTrue(self.request("CD_STOP").startswith("OK CD_TRANSPORT_TX"))
        stopped = time.monotonic()
        reply = self.settle()
        self.assertTrue(reply.startswith("OK QUERY_STATUS reason=NONE "), reply)
        self.assertNotIn("0x94", reply)
        self.assertGreaterEqual(time.monotonic() - stopped, 0.6 + 0.5)
        waited = [int(ms) for ms in re.findall(r"settle waited_ms=(\d+)",
                                               self.log_path.read_text())]
        self.assertEqual(len(waited), 1)
        self.assertGreaterEqual(waited[0], 550)
        self.assertEqual(self.status()["receiver_source"], "CD")
        self.assertEqual(self.transmitted(), ["0xA1", "0x13", "0xA1", "0x13"])

    def test_settle_times_out_under_continuous_traffic_without_tx(self):
        traffic = " ".join(f"+{150000 * i}:0x94" for i in range(1, 17))
        self.rx("0xA1 echo " + traffic, QUERY_CD_ON)
        self.assertTrue(self.request("CD_STOP").startswith("OK CD_TRANSPORT_TX"))
        started = time.monotonic()
        with self.connect() as owner:
            owner.sendall(b"QUERY_STATUS SETTLE\n")
            self.assertEqual(self.read_reply(owner), "ERR QUERY_STATUS reason=BUS_ACTIVE")
        elapsed = time.monotonic() - started
        self.assertTrue(1.4 <= elapsed < 2.2, elapsed)
        self.assertEqual(self.transmitted(), ["0xA1"], "nothing sent on a busy bus")
        self.assertIn("XS8 QUERY_STATUS settle ERR QUERY_STATUS reason=BUS_ACTIVE",
                      self.log_path.read_text())
        # Once the traffic has ended, a new request (never an automatic retry) works.
        time.sleep(1.2)
        self.assertTrue(self.settle().startswith("OK QUERY_STATUS"))
        self.assertEqual(self.transmitted(), ["0xA1", "0x13"])

    def test_volume_press_preempts_a_pending_settle(self):
        self.rx("0xA1 echo", QUERY_CD_ON)
        self.assertTrue(self.request("CD_STOP").startswith("OK CD_TRANSPORT_TX"))
        with self.connect() as owner, self.connect() as cec:
            owner.sendall(b"QUERY_STATUS SETTLE\n")
            time.sleep(0.05)
            # Still settling: other controls are refused, STATUS and RELEASE are not.
            for line in ("QUERY_STATUS", "QUERY_STATUS SETTLE", "CD_STOP",
                         "SELECT_SOURCE CD", "VOLUME_STEP UP", "RESEARCH_SEND 0x9C 100"):
                self.assertEqual(self.command(cec, line),
                                 "ERR QUERY_STATUS reason=SESSION_BUSY", line)
            self.assertTrue(self.command(cec, "STATUS").startswith("OK state=IDLE"))
            # A CEC volume press is not refused: the settle ends with nothing sent.
            self.assertEqual(self.command(cec, "VOLUME_PRESS UP"), "OK VOLUME_PRESS_TX")
            self.assertEqual(self.read_reply(owner), "ERR QUERY_STATUS reason=PREEMPTED")
            self.assertEqual(self.command(cec, "VOLUME_RELEASE UP"), "OK VOLUME_RELEASE_TX")
        self.assertNotIn("0x13", self.transmitted())
        self.assertIn("settle preempted by VOLUME_PRESS; nothing sent",
                      self.log_path.read_text())

    def test_mute_preempts_a_pending_settle(self):
        self.rx("0xA1 echo", QUERY_CD_ON)
        self.assertTrue(self.request("CD_STOP").startswith("OK CD_TRANSPORT_TX"))
        with self.connect() as owner, self.connect() as cec:
            owner.sendall(b"QUERY_STATUS SETTLE\n")
            time.sleep(0.05)
            # Only an exact MUTE preempts; anything else is still refused.
            self.assertEqual(self.command(cec, "MUTE NOW"),
                             "ERR QUERY_STATUS reason=SESSION_BUSY")
            reply = self.command(cec, "MUTE")
            self.assertTrue(reply.startswith("OK MUTE_TX"), reply)
            self.assertEqual(self.read_reply(owner), "ERR QUERY_STATUS reason=PREEMPTED")
        sent = self.transmitted()
        self.assertNotIn("0x13", sent)
        self.assertEqual(sent[0], "0xA1")
        self.assertGreater(len(sent), 1, "MUTE followed its own control path")
        self.assertIn("settle preempted by MUTE; nothing sent", self.log_path.read_text())
        self.assertTrue(self.request("STATUS").startswith("OK state=IDLE"))

    def test_mute_preemption_keeps_mute_gates(self):
        # A preempting MUTE is still refused by its own gates (here a CD hold):
        # the settle ends, MUTE sends nothing and nothing is retried.
        self.rx(QUERY_CD_ON)
        token = self.press("SEEK_FORWARD")
        before = self.transmitted()
        with self.connect() as owner, self.connect() as cec:
            owner.sendall(b"QUERY_STATUS SETTLE\n")
            time.sleep(0.05)
            self.assertEqual(self.command(cec, "MUTE"), "ERR CD_TRACK_BUSY")
            self.assertEqual(self.read_reply(owner), "ERR QUERY_STATUS reason=PREEMPTED")
        self.assertEqual(self.transmitted(), before)
        self.assertTrue(self.request("CD_RELEASE " + token).startswith("OK CD_RELEASE_TX"))

    def test_once_transmitted_the_query_stays_exclusive(self):
        self.rx(QUERY_CD_ON)
        with self.connect() as owner, self.connect() as cec:
            owner.sendall(b"QUERY_STATUS SETTLE\n")
            deadline = time.monotonic() + 1
            while "0x13" not in self.transmitted():
                self.assertLess(time.monotonic(), deadline, "query never transmitted")
                time.sleep(0.01)
            for line in ("VOLUME_PRESS UP", "MUTE"):
                self.assertEqual(self.command(cec, line),
                                 "ERR QUERY_STATUS reason=SESSION_BUSY", line)
            self.assertTrue(self.read_reply(owner).startswith("OK QUERY_STATUS reason=NONE"))
        self.assertEqual(self.transmitted(), ["0x13"])
        self.assertNotIn("preempted", self.log_path.read_text())

    def test_settle_owner_disconnect_or_input_sends_nothing(self):
        self.rx("0xA1 echo", QUERY_CD_ON)
        self.assertTrue(self.request("CD_STOP").startswith("OK CD_TRANSPORT_TX"))
        with self.connect() as owner:
            owner.sendall(b"QUERY_STATUS SETTLE\n")
            time.sleep(0.05)
        with self.connect() as owner:
            owner.sendall(b"QUERY_STATUS SETTLE\n")
            time.sleep(0.05)
            owner.sendall(b"STATUS\n")  # Any owner input ends its pending request.
            owner.settimeout(1)
            self.assertEqual(owner.recv(1024), b"")
        time.sleep(0.9)
        self.assertEqual(self.transmitted(), ["0xA1"])
        self.assertTrue(self.request("STATUS").startswith("OK state=IDLE"))

    def test_local_source_and_monitor_tx_clear_the_current_observation(self):
        self.rx(QUERY_CD_ON)
        self.assertTrue(self.request("QUERY_STATUS").startswith("OK QUERY_STATUS"))
        # Refused before any TX: nothing is cleared.
        self.assertFalse(self.request("SELECT_SOURCE BOGUS").startswith("OK"))
        self.assertEqual(self.status()["receiver_source"], "CD")
        self.assertEqual(self.request("SELECT_SOURCE TUNER").split()[0], "OK")
        status = self.status()
        self.assertEqual((status["receiver_source"], status["receiver_source_ns"],
                          status["receiver_source_evidence"]), ("UNKNOWN", "0", "NONE"))
        self.assertEqual(status["receiver_monitor"], "ON")
        self.assertEqual(status["receiver_integrity"], "HEALTHY")
        self.assertRegex(self.log_path.read_text(),
                         r"XS8 RECEIVER source cleared after local TX floor_ns=\d+ "
                         r"last_source=CD last_source_ns=[1-9]\d* last_source_evidence=QUERY")
        self.rx(QUERY_TUNER_ON)
        self.assertIn("source_inferred=TUNER", self.settle())
        self.assertEqual(self.status()["receiver_source"], "TUNER")
        self.assertTrue(self.request("TAPE2_MONITOR_TOGGLE").startswith("OK TAPE2_MONITOR_TX"))
        status = self.status()
        self.assertEqual((status["receiver_monitor"], status["receiver_monitor_ns"],
                          status["receiver_monitor_evidence"]), ("UNKNOWN", "0", "NONE"))
        self.assertEqual(status["receiver_source"], "TUNER")
        self.assertIn("XS8 RECEIVER monitor cleared after local TX", self.log_path.read_text())


class CdProbeSocketTest(ScriptedRxSocketTest):
    """Experimental PROBE_CD_CHANGER with simulated RX timed from the fake TX."""

    extra_args = ("--enable-cd-presence-probe",)

    def probe(self, rx):
        time.sleep(1.05)  # The service's 1 s guard after any local TX.
        self.rx(rx)
        return self.request("PROBE_CD_CHANGER")

    def test_response_about_three_ms_after_tx(self):
        self.assertIn("cd_presence_probe=EXPERIMENTAL", self.log_path.read_text())
        before = self.status()
        self.assertEqual((before["cd_changer"], before["cd_changer_ns"],
                          before["cd_changer_last_probe"]), ("UNKNOWN", "0", "NONE"))
        self.rx("0xC3 echo +3000:0x94")
        reply = self.request("PROBE_CD_CHANGER")
        self.assertRegex(reply, r"^OK CD_PROBE result=RESPONSE latency=\d+us frames=1 "
                                r"rx=0x94@\+\d+us gpio_tx=TX_OK waveform=MATCH "
                                r"receiver_execution=UNVERIFIED$")
        latency = int(re.search(r"latency=(\d+)us", reply).group(1))
        self.assertTrue(1000 <= latency <= 3000, latency)
        self.assertEqual(self.transmitted(), ["0xC3"], "exactly one frame, no retry")
        after = self.status()
        self.assertEqual((after["cd_changer"], after["cd_changer_last_probe"]),
                         ("PRESENT", "RESPONSE"))
        self.assertNotEqual(after["cd_changer_ns"], "0")
        # Presence never touches the receiver source observation.
        self.assertEqual((after["receiver_source"], after["receiver_source_evidence"]),
                         ("UNKNOWN", "NONE"))

    def test_silence_is_no_response_not_off(self):
        self.rx("0xC3 echo")
        self.assertEqual(self.request("PROBE_CD_CHANGER"),
                         "OK CD_PROBE result=NO_RESPONSE latency=NONE frames=0 rx=NONE "
                         "gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED")
        status = self.status()
        self.assertEqual((status["cd_changer"], status["cd_changer_last_probe"]),
                         ("NOT_DETECTED", "NO_RESPONSE"))
        self.assertNotIn("ABSENT", self.request("STATUS"))

    def test_multiple_and_late_frames_are_ambiguous_and_change_nothing(self):
        self.rx("0xC3 echo +3000:0x94")
        self.assertIn("result=RESPONSE", self.request("PROBE_CD_CHANGER"))
        for rx, frames in (("0xC3 echo +3000:0x94 +30000:0x94", "frames=2"),
                           ("0xC3 echo +35000:0x94", "frames=1"),
                           ("0xC3 echo +3000:0xE4", "frames=1")):
            with self.subTest(rx=rx):
                reply = self.probe(rx)
                self.assertTrue(reply.startswith("OK CD_PROBE result=AMBIGUOUS"), reply)
                self.assertIn(frames, reply)
                status = self.status()
                # The earlier RESPONSE stays the observation; nothing is inferred.
                self.assertEqual((status["cd_changer"], status["cd_changer_last_probe"]),
                                 ("PRESENT", "AMBIGUOUS"))
        self.assertEqual(self.transmitted(), ["0xC3"] * 4)

    def test_faults_fail_without_establishing_presence(self):
        for rx, reason in (("0xC3 echo +3000:INVALID", "INVALID_FRAME"),
                           ("0xC3 echo +3000:0x94 +4000:GAP", "EVENT_GAP"),
                           ("0xC3 +0:0x94", "TX_MISMATCH"),
                           ("0xC3", "TX_MISSING"),
                           ("0xC3 echo +0:0x94", "OVERLAP")):
            with self.subTest(reason=reason):
                reply = self.probe(rx)
                self.assertTrue(reply.startswith("ERR CD_PROBE reason=" + reason), reply)
                self.assertTrue(reply.endswith("receiver_execution=UNVERIFIED"), reply)
                status = self.status()
                self.assertEqual((status["cd_changer"], status["cd_changer_last_probe"]),
                                 ("UNKNOWN", "FAILED"))
        self.assertEqual(self.transmitted(), ["0xC3"] * 5, "never retried")

    def test_recent_tx_and_bus_activity_guards(self):
        self.rx("0xC3 echo")
        self.assertIn("NO_RESPONSE", self.request("PROBE_CD_CHANGER"))
        self.assertEqual(self.request("PROBE_CD_CHANGER"), "ERR CD_PROBE reason=RECENT_TX")
        # A delayed report from an earlier CD operation: refused until the bus
        # has been quiet, so it can never land in a probe window.
        time.sleep(1.05)
        self.rx("0xA1 +1100000:0x94")
        self.assertTrue(self.request("CD_STOP").startswith("OK CD_TRANSPORT_TX"))
        self.assertEqual(self.request("PROBE_CD_CHANGER"), "ERR CD_PROBE reason=RECENT_TX")
        time.sleep(1.25)  # The delayed 0x94 has now arrived.
        self.assertEqual(self.request("PROBE_CD_CHANGER"), "ERR CD_PROBE reason=BUS_ACTIVE")
        self.assertEqual(self.transmitted(), ["0xC3", "0xA1"])
        time.sleep(0.55)
        self.rx("0xC3 echo")
        self.assertIn("NO_RESPONSE", self.request("PROBE_CD_CHANGER"))
        self.assertEqual(self.transmitted(), ["0xC3", "0xA1", "0xC3"])

    def test_busy_states_are_mutually_exclusive(self):
        # A pending probe refuses every other operation; STATUS stays available.
        self.rx("0xC3 echo +3000:0x94")
        with self.connect() as owner, self.connect() as other:
            owner.sendall(b"PROBE_CD_CHANGER\n")
            time.sleep(0.01)
            for line in ("QUERY_STATUS", "CD_STOP", "PROBE_CD_CHANGER", "SELECT_SOURCE CD",
                         "VOLUME_STEP UP", "RESEARCH_SEND 0x9C 100"):
                self.assertEqual(self.command(other, line), "ERR CD_PROBE reason=SESSION_BUSY")
            self.assertTrue(self.command(other, "STATUS").startswith("OK state=IDLE"))
            # Reading only: any input from the owner would cancel its probe.
            owner.settimeout(2)
            reply = bytearray()
            while not reply.endswith(b"\n"):
                chunk = owner.recv(1024)
                self.assertTrue(chunk, "service closed before its reply")
                reply.extend(chunk)
            self.assertIn("result=RESPONSE", reply.decode("ascii"))
        self.assertEqual(self.transmitted(), ["0xC3"])
        # A research capture or a CD hold refuses the probe; nothing is sent.
        time.sleep(1.05)
        self.assertTrue(self.request("RESEARCH_SEND 0x9C 3000").startswith("OK RESEARCH"))
        time.sleep(1.05)
        self.assertEqual(self.request("PROBE_CD_CHANGER"), "ERR CD_PROBE reason=SESSION_BUSY")
        time.sleep(2.0)
        token = self.press("SEEK_FORWARD")
        time.sleep(0.6)
        self.assertTrue(self.request("CD_HEARTBEAT " + token).startswith("OK"))
        time.sleep(0.5)
        self.assertEqual(self.request("PROBE_CD_CHANGER"), "ERR CD_TRACK_BUSY")
        self.assertTrue(self.request("CD_RELEASE " + token).startswith("OK CD_RELEASE_TX"))
        self.assertEqual(self.transmitted(), ["0xC3", "0x9C", "0x7A", "0x72"])

    def test_disconnect_cancels_without_changing_presence(self):
        self.rx("0xC3 echo +3000:0x94")
        with self.connect() as client:
            client.sendall(b"PROBE_CD_CHANGER\n")
            time.sleep(0.01)
        time.sleep(0.2)
        status = self.status()
        self.assertEqual((status["cd_changer"], status["cd_changer_last_probe"]),
                         ("UNKNOWN", "NONE"))
        self.assertEqual(self.transmitted(), ["0xC3"])
        # The service stays usable; the next probe is a new, explicit request.
        self.assertIn("result=RESPONSE", self.probe("0xC3 echo +3000:0x94"))
        self.assertEqual(self.transmitted(), ["0xC3", "0xC3"])


if __name__ == "__main__":
    unittest.main()
