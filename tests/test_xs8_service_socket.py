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


if __name__ == "__main__":
    unittest.main()
