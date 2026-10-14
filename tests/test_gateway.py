import http.client
import contextlib
import io
import json
import os
import re
import socket
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from html.parser import HTMLParser
from pathlib import Path
from unittest import mock

import xs8_gateway


STATUS = (b"OK state=IDLE stop_owed=0 context_may_be_open=0 "
          b"monitor=HEALTHY last_byte=0x09 last_gpio_tx=TX_OK "
          b"last_waveform=MATCH last_stop_waveform=MATCH "
          b"receiver_execution=UNVERIFIED\n")
RECEIVER_STATUS = STATUS.replace(b"\n", (
    b" receiver_source=CD receiver_source_ns=1000000000"
    b" receiver_source_evidence=PASSIVE receiver_monitor=ON"
    b" receiver_monitor_ns=1000000100 receiver_monitor_evidence=QUERY"
    b" receiver_integrity=HEALTHY\n"))
CD_HOLD_STATUS = STATUS.replace(b"\n", (
    b" cd_state=HOLDING cd_action=TRACK_NEXT cd_release_owed=1"
    b" cd_lease_expired=0 cd_last_result=HOLDING\n"))
CD_IDLE_STATUS = STATUS.replace(b"\n", (
    b" cd_state=IDLE cd_action=NONE cd_release_owed=0"
    b" cd_lease_expired=0 cd_last_result=NONE\n"))
CD_FAULT_STATUS = STATUS.replace(b"state=IDLE", b"state=FAULT_LATCHED").replace(
    b"\n", (b" cd_state=FAULT_LATCHED cd_action=NEXT cd_release_owed=1"
            b" cd_lease_expired=0 cd_last_result=RELEASE_UNCERTAIN\n"))
CD_OK = (b"OK CD_TRACK_TX action=NEXT press_gpio_tx=TX_OK press_waveform=MATCH "
         b"release_gpio_tx=TX_OK release_waveform=MATCH cd_result=TX_VERIFIED "
         b"receiver_execution=UNVERIFIED\n")
CD_UNCERTAIN = (b"ERR CD_TRACK_UNCERTAIN action=NEXT press_gpio_tx=TX_OK "
                b"press_waveform=MATCH release_gpio_tx=TX_OK release_waveform=MISSING "
                b"cd_result=RELEASE_UNCERTAIN receiver_execution=UNVERIFIED\n")
VOLUME_OK = (b"OK VOLUME_STEP_TX direction=UP state=IDLE stop_owed=0 "
             b"context_may_be_open=0 receiver_execution=UNVERIFIED\n")
VOLUME_UNCERTAIN = (b"ERR VOLUME_STEP_UNCERTAIN direction=UP state=FAULT_LATCHED "
                    b"stop_owed=1 context_may_be_open=1 receiver_execution=UNVERIFIED\n")
CD_TRANSPORT_OK = (b"OK CD_TRANSPORT_TX action=STOP gpio_tx=TX_OK waveform=MATCH "
                   b"receiver_execution=UNVERIFIED\n")
QUERY_REPLY = (b"OK QUERY_STATUS reason=NONE gpio_tx=TX_OK waveform=MATCH "
               b"bytes=lsb/legacy@start_ns-end_ns:validity "
               b"tx=0x13/0x37@1-2:VALID rx1=0x47/0x1D@3-4:VALID "
               b"rx2=0xD7/0x14@5-6:VALID gap_events=0 "
               b"source_inferred=CD monitor_inferred=ON "
               b"attribution=TEMPORAL origin=UNATTRIBUTED "
               b"receiver_execution=UNVERIFIED\n")


class FakeService:
    def __init__(self, path, response, delay=0):
        self.path = path
        self.response = response
        self.delay = delay
        self.commands = []
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(path)
        self.listener.listen(1)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        client, _ = self.listener.accept()
        with client:
            self.commands.append(client.recv(128))
            if self.delay:
                time.sleep(self.delay)
            if self.response is not None:
                try:
                    client.sendall(self.response)
                except BrokenPipeError:
                    pass

    def close(self):
        self.thread.join(timeout=2)
        self.listener.close()
        Path(self.path).unlink(missing_ok=True)
        if self.thread.is_alive():
            raise AssertionError("fake service did not receive a connection")


class OfflineFakeService(FakeService):
    """Answer the fake-only challenge before accepting one MUTE."""

    def run(self):
        client, _ = self.listener.accept()
        with client:
            challenge = bytearray()
            while b"\n" not in challenge:
                chunk = client.recv(128)
                if not chunk:
                    return
                challenge.extend(chunk)
            challenge = bytes(challenge)
            self.commands.append(challenge)
            if not challenge.startswith(b"OFFLINE_TEST ") or not challenge.endswith(b"\n"):
                return
            client.sendall(b"OK " + challenge)
            command = bytearray()
            while b"\n" not in command:
                chunk = client.recv(128)
                if not chunk:
                    return
                command.extend(chunk)
            self.commands.append(bytes(command))
            if self.delay:
                time.sleep(self.delay)
            if self.response is not None:
                try:
                    client.sendall(self.response)
                except BrokenPipeError:
                    pass


class RecordingService:
    """Multi-connection fake for production-mode STATUS and MUTE requests."""

    def __init__(self, path, status=STATUS, mute_reply=b"OK MUTE_TX\n",
                 query_reply=QUERY_REPLY):
        self.path = path
        self.status = status
        self.mute_reply = mute_reply
        self.query_reply = query_reply
        self.commands = []
        self.stop = threading.Event()
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(path)
        self.listener.listen(4)
        self.listener.settimeout(0.1)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop.is_set():
            try:
                client, _ = self.listener.accept()
            except socket.timeout:
                continue
            with client:
                command = client.recv(128)
                self.commands.append(command)
                reply = (self.status if command == b"STATUS\n" else
                         self.query_reply if command == b"QUERY_STATUS\n" else
                         self.mute_reply)
                if reply is not None:
                    try:
                        client.sendall(reply)
                    except BrokenPipeError:
                        pass

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        self.listener.close()
        Path(self.path).unlink(missing_ok=True)
        if self.thread.is_alive():
            raise AssertionError("recording fake service did not stop")


class KeyService:
    """Fake XS8 Service with connection-owned KEY_DOWN/KEY_UP semantics.

    One thread per connection. The connection that pressed owns the single CD
    hold; KEY_UP or closing that connection releases it (0x72). A service-side
    expiry blocks that connection's KEY_DOWN with KEY_EXPIRED. STATUS reports
    the hold. `script[verb]` can queue raw replies (bytes), None (close without
    replying) or ("delay", seconds, bytes)."""

    PRESS = {"SEEK_FORWARD": ["0x7A"], "SEEK_BACKWARD": ["0x6A"],
             "VOLUME_UP": ["0x17", "0xF9"], "VOLUME_DOWN": ["0x17", "0xE9"]}

    def __init__(self, path, status=CD_IDLE_STATUS):
        self.path = path
        self.status = status
        self.lock = threading.Lock()
        self.tx = []
        self.events = []
        self.hold = None
        self.expired = {}
        self.last = "NONE"
        self.script = {}
        self.sockets = {}
        self.threads = []
        self.next_id = 0
        self.stop = threading.Event()
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(path)
        self.listener.listen(16)
        self.listener.settimeout(0.05)
        self.thread = threading.Thread(target=self.accept_loop, daemon=True)
        self.thread.start()

    def accept_loop(self):
        while not self.stop.is_set():
            try:
                client, _ = self.listener.accept()
            except (socket.timeout, OSError):
                continue
            with self.lock:
                self.next_id += 1
                cid = self.next_id
                self.sockets[cid] = client
            thread = threading.Thread(target=self.serve, args=(cid, client), daemon=True)
            self.threads.append(thread)
            thread.start()

    def serve(self, cid, client):
        buffer = b""
        try:
            while True:
                try:
                    chunk = client.recv(256)
                except OSError:
                    break
                if not chunk:
                    break
                buffer += chunk
                while b"\n" in buffer:
                    raw, buffer = buffer.split(b"\n", 1)
                    reply = self.handle(cid, raw.decode())
                    if isinstance(reply, tuple):
                        time.sleep(reply[1])
                        reply = reply[2]
                    if reply is None:
                        return
                    try:
                        client.sendall(reply)
                    except OSError:
                        return
        finally:
            client.close()
            self.closed(cid)

    def handle(self, cid, line):
        with self.lock:
            self.events.append((cid, line))
            verb = line.split(" ", 1)[0]
            if self.script.get(verb):
                return self.script[verb].pop(0)
            if line == "STATUS":
                if self.hold and self.hold["action"].startswith("VOLUME_"):
                    return CD_IDLE_STATUS.replace(
                        b"OK state=IDLE", b"OK state=HOLDING_" + self.hold["action"][7:].encode())
                return (CD_HOLD_STATUS.replace(b"cd_action=TRACK_NEXT",
                                               b"cd_action=" + self.hold["action"].encode())
                        if self.hold else self.status)
            action = line.split(" ", 1)[1] if " " in line else ""
            if verb == "KEY_DOWN":
                if action not in self.PRESS:
                    return b"ERR KEY_UNKNOWN_ACTION\n"
                if self.hold and self.hold["conn"] == cid and self.hold["action"] == action:
                    return f"OK KEY_ALIVE action={action}\n".encode()
                if self.hold:
                    return b"ERR KEY_BUSY\n"
                if cid in self.expired:
                    return f"ERR KEY_EXPIRED last={self.expired[cid]}\n".encode()
                self.hold = {"conn": cid, "action": action}
                self.tx.extend(self.PRESS[action])
                return (f"OK KEY_DOWN_TX action={action} gpio_tx=TX_OK waveform=MATCH "
                        "receiver_execution=UNVERIFIED\n").encode()
            if verb == "KEY_UP":
                if self.hold and self.hold["conn"] == cid:
                    if self.hold["action"] != action:
                        return b"ERR KEY_ACTION_MISMATCH\n"
                    self.release("CLIENT_RELEASE")
                    return (f"OK KEY_UP_TX action={action} gpio_tx=TX_OK waveform=MATCH "
                            "receiver_execution=UNVERIFIED\n").encode()
                if self.hold:
                    return b"ERR KEY_NOT_OWNER\n"
                self.expired.pop(cid, None)
                return f"OK KEY_UP_NONE last={self.last}\n".encode()
            return b"ERR UNKNOWN_COMMAND\n"

    def release(self, reason):
        # CD KEY UP, or volume STOP then CONTEXT exit.
        self.tx.extend(["0x72"] if self.hold["action"].startswith("SEEK_") else ["0x09", "0x07"])
        self.last = reason
        owner = self.hold["conn"]
        self.hold = None
        return owner

    def closed(self, cid):
        with self.lock:
            self.events.append((cid, "CLOSE"))
            if self.hold and self.hold["conn"] == cid:
                self.release("CLIENT_DISCONNECT")
            self.expired.pop(cid, None)

    def expire(self, reason):
        """Service-side INACTIVITY or MAX_HOLD release."""
        with self.lock:
            owner = self.release(reason)
            self.expired[owner] = reason

    def drop(self, cid):
        """The service side closes a connection (for example, a service restart)."""
        self.sockets[cid].shutdown(socket.SHUT_RDWR)

    def key_connections(self):
        with self.lock:
            return sorted({cid for cid, line in self.events if line.startswith("KEY_")})

    def lines(self, cid=None):
        """Commands (no STATUS) on every connection, or one connection's
        commands and its CLOSE."""
        with self.lock:
            if cid is None:
                return [line for c, line in self.events if line not in ("STATUS", "CLOSE")]
            return [line for c, line in self.events if c == cid and line != "STATUS"]

    def wait(self, predicate, timeout=4.0):
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise AssertionError("condition not reached; events: %r" % self.events)
            time.sleep(0.02)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        self.listener.close()
        for client in list(self.sockets.values()):
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        for thread in self.threads:
            thread.join(timeout=2)
        Path(self.path).unlink(missing_ok=True)


class DisabledControls(HTMLParser):
    def __init__(self):
        super().__init__()
        self.buttons = []

    def handle_starttag(self, tag, attrs):
        if tag == "button":
            self.buttons.append(dict(attrs))


class GatewayTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        socket_path = str(Path(self.temp.name) / "xs8.sock")
        self.socket_path = socket_path
        handler = type("TestHandler", (xs8_gateway.GatewayHandler,),
                       {"socket_path": socket_path,
                        "dev_mute_enabled": False, "https_origin": None, "sessions": {},
                        "log_message": lambda *args: None})
        self.server = xs8_gateway.GatewayHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.thread.join(timeout=2)
        self.server.server_close()
        self.temp.cleanup()

    def request_full(self, method, path, body=None, headers=None):
        client = http.client.HTTPConnection("127.0.0.1", self.server.server_port,
                                            timeout=2)
        client.request(method, path, body=body, headers=headers or {})
        response = client.getresponse()
        result = response.status, response.read(), dict(response.getheaders())
        client.close()
        return result

    def request_with_headers(self, method, path, headers, body=None):
        """Preserve duplicate headers for HTTP parser regression tests."""
        client = http.client.HTTPConnection("127.0.0.1", self.server.server_port,
                                            timeout=2)
        client.putrequest(method, path)
        for name, value in headers:
            client.putheader(name, value)
        client.endheaders(body.encode() if body is not None else None)
        response = client.getresponse()
        result = response.status, response.read()
        client.close()
        return result

    def request(self, method, path):
        code, body, _ = self.request_full(method, path)
        return code, body

    def enable_dev_mute(self):
        handler = self.server.RequestHandlerClass
        handler.dev_mute_enabled = True
        handler.https_origin = "https://woodland-pi.local"
        # Handler-only regression tests simulate a verified proxy. The CLI gate is off.
        self.server.proxy_peer_verified = True

    def session(self):
        code, body, headers = self.request_full(
            "GET", "/api/v1/session", headers={"X-Kenwood-Authenticated-User": "remote"})
        self.assertEqual(code, 200)
        return json.loads(body)["csrf_token"], headers["Set-Cookie"].split(";", 1)[0]

    def mute(self, token, cookie, **overrides):
        headers = {"Origin": "https://woodland-pi.local", "X-Forwarded-Proto": "https",
                   "X-Kenwood-Authenticated-User": "remote", "X-CSRF-Token": token,
                   "Cookie": cookie, "Content-Type": "application/json"}
        headers.update(overrides.pop("headers", {}))
        return self.request_full("POST", "/api/v1/mute",
                                 body=overrides.pop("body", "{}"), headers=headers)

    def source(self, token, cookie, name, **overrides):
        headers = {"Origin": "https://woodland-pi.local", "X-Forwarded-Proto": "https",
                   "X-Kenwood-Authenticated-User": "remote", "X-CSRF-Token": token,
                   "Cookie": cookie, "Content-Type": "application/json"}
        headers.update(overrides.pop("headers", {}))
        return self.request_full("POST", "/api/v1/source",
                                 body=overrides.pop("body", json.dumps({"source": name})),
                                 headers=headers)

    def test_status_parsing(self):
        fake = FakeService(self.socket_path, STATUS)
        code, body = self.request("GET", "/api/v1/status")
        fake.close()
        self.assertEqual(code, 200)
        self.assertEqual(fake.commands, [b"STATUS\n"])
        parsed = json.loads(body)
        self.assertEqual(parsed["status"]["state"], "IDLE")
        self.assertEqual(parsed["status"]["last_gpio_tx"], "TX_OK")
        self.assertEqual(parsed["status"]["receiver_execution"], "UNVERIFIED")

    def test_cd_hold_status_remains_available_but_controls_are_ineligible(self):
        fake = FakeService(self.socket_path, CD_HOLD_STATUS)
        code, body = self.request("GET", "/api/v1/status")
        fake.close()
        self.assertEqual(code, 200)
        self.assertEqual(fake.commands, [b"STATUS\n"])
        status = json.loads(body)["status"]
        self.assertEqual(status["cd_state"], "HOLDING")
        self.assertFalse(xs8_gateway.GatewayHandler.status_allows_mute(status))

    def test_invalid_service_reply(self):
        fake = FakeService(self.socket_path, b"OK state=IDLE\n")
        code, body = self.request("GET", "/api/v1/status")
        fake.close()
        self.assertEqual(code, 503)
        self.assertIn("Invalid XS8 service STATUS reply", json.loads(body)["error"])

    def test_unavailable_service(self):
        code, body = self.request("GET", "/api/v1/status")
        self.assertEqual(code, 503)
        self.assertEqual(json.loads(body)["xs8_service"], "unavailable")

    def test_controls_are_disabled_and_post_is_inert(self):
        code, body = self.request("GET", "/")
        self.assertEqual(code, 200)
        parser = DisabledControls()
        parser.feed(body.decode())
        self.assertEqual(len(parser.buttons), 20)
        self.assertTrue(all("disabled" in button for button in parser.buttons))
        for path in ("/api/v1/source", "/api/v1/mute",
                     "/api/v1/receiver/query",
                     "/api/v1/volume/step", "/api/v1/cd/transport",
                     "/api/v1/cd/track", "/api/v1/cd/playback", "/api/v1/cd/seek",
                     "/api/v1/volume/hold", "/api/v1/power", "/api/v1/receiver/monitor",
                     "/api/v1/status", "/anything"):
            code, _ = self.request("POST", path)
            self.assertEqual(code, 501)
        code, body = self.request("GET", "/api/v1/capabilities")
        self.assertEqual(code, 200)
        operations = json.loads(body)["operations"]
        self.assertTrue(operations["status"]["enabled"])
        self.assertTrue(all(not value["enabled"] for key, value in
                            operations.items() if key != "status"))


class MuteGatewayTest(GatewayTest):
    def test_success_one_command_and_replay_rejected(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        fake = OfflineFakeService(self.socket_path, b"OK MUTE_TX\n")
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 200)
        self.assertEqual(fake.commands[-1], b"MUTE\n")
        self.assertEqual(json.loads(body)["receiver_execution"], "UNVERIFIED")
        self.assertEqual(json.loads(body)["result"], "SERVICE_TX_OK")
        self.assertEqual(self.mute(token, cookie)[0], 403)

    def test_service_uncertain_is_not_labeled_rejected(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        fake = OfflineFakeService(self.socket_path, b"ERR MUTE_UNCERTAIN\n")
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 502)
        self.assertEqual(fake.commands[-1], b"MUTE\n")
        self.assertEqual(json.loads(body)["result"], "SERVICE_UNCERTAIN")

    def test_boot_inhibit_not_shown_as_available(self):
        self.enable_dev_mute()
        fake = FakeService(self.socket_path, STATUS.replace(b"state=IDLE", b"state=BOOT_INHIBIT"))
        code, body, headers = self.request_full(
            "GET", "/api/v1/session", headers={"X-Kenwood-Authenticated-User": "remote"})
        fake.close()
        self.assertEqual(code, 200)
        self.assertFalse(json.loads(body)["mute_available"])
        token = json.loads(body)["csrf_token"]
        cookie = headers["Set-Cookie"].split(";", 1)[0]
        fake = OfflineFakeService(self.socket_path, b"ERR MUTE_UNCERTAIN\n")
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 502)
        self.assertEqual(fake.commands[-1], b"MUTE\n")
        self.assertEqual(json.loads(body)["result"], "SERVICE_UNCERTAIN")

    def test_invalid_service_reply_is_not_success(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        fake = OfflineFakeService(self.socket_path, b"OK OTHER\n")
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 502)
        self.assertEqual(json.loads(body)["result"], "INVALID_REPLY")

    def test_unavailable_timeout_and_disconnect(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        code, body, _ = self.mute(token, cookie)
        self.assertEqual(code, 503)
        self.assertEqual(json.loads(body)["result"], "UNAVAILABLE")
        token, cookie = self.session()
        fake = OfflineFakeService(self.socket_path, None, delay=1.2)
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 504)
        self.assertEqual(json.loads(body)["result"], "TIMEOUT")
        token, cookie = self.session()
        fake = OfflineFakeService(self.socket_path, None)
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 502)
        self.assertEqual(json.loads(body)["result"], "DISCONNECTED")

    def test_rejected_requests_never_contact_service(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        self.assertEqual(self.request("POST", "/api/v1/mute")[0], 403)
        self.assertEqual(self.mute(token, cookie, headers={"Origin": "https://attacker.example"})[0], 403)
        self.assertEqual(self.mute(token, cookie, headers={"X-CSRF-Token": "wrong"})[0], 403)
        self.assertEqual(self.mute(token, cookie, headers={"Cookie": "wrong=1"})[0], 401)
        self.assertEqual(self.mute(token, cookie, headers={"X-Kenwood-Authenticated-User": ""})[0], 401)
        self.assertEqual(self.mute(token, cookie, headers={"Content-Type": "text/plain"})[0], 400)
        self.assertEqual(self.mute(token, cookie, body='{"x":1}')[0], 400)
        for path in ("/api/v1/source", "/api/v1/volume/step", "/api/v1/cd/transport"):
            self.assertEqual(self.request("POST", path)[0], 501)
        # A valid request is still accepted: rejected requests did not use the token.
        fake = OfflineFakeService(self.socket_path, b"OK MUTE_TX\n")
        self.assertEqual(self.mute(token, cookie)[0], 200)
        fake.close()
        self.assertEqual(fake.commands[-1], b"MUTE\n")

    def test_duplicate_and_missing_headers_never_dispatch(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        valid = [
            ("Origin", "https://woodland-pi.local"),
            ("X-Forwarded-Proto", "https"),
            ("X-Kenwood-Authenticated-User", "remote"),
            ("X-CSRF-Token", token),
            ("Cookie", cookie),
            ("Content-Type", "application/json"),
            ("Content-Length", "2"),
        ]
        with mock.patch.object(xs8_gateway, "send_mute") as dispatch:
            for name in ("Origin", "X-Forwarded-Proto", "X-CSRF-Token",
                         "Cookie", "Content-Type", "Content-Length"):
                with self.subTest(missing=name):
                    headers = [(key, value) for key, value in valid if key != name]
                    self.assertNotEqual(self.request_with_headers(
                        "POST", "/api/v1/mute", headers, "{}")[0], 200)
                with self.subTest(duplicate=name):
                    self.assertNotEqual(self.request_with_headers(
                        "POST", "/api/v1/mute", valid + [(name, dict(valid)[name])],
                        "{}")[0], 200)
            for name, value in (("Transfer-Encoding", "chunked"),
                                ("X-Kenwood-Authenticated-User", "intruder")):
                with self.subTest(extra=name):
                    self.assertNotEqual(self.request_with_headers(
                        "POST", "/api/v1/mute", valid + [(name, value)],
                        "{}")[0], 200)
            dispatch.assert_not_called()

    def test_session_bound_to_identity_and_token_consumed_on_service_error(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "send_mute") as dispatch:
            self.assertEqual(self.mute(token, cookie, headers={
                "X-Kenwood-Authenticated-User": "other"})[0], 401)
            self.assertEqual(self.mute(token, cookie, headers={
                "Origin": "https://woodland-pi.local:443"})[0], 403)
            dispatch.assert_not_called()
        fake = OfflineFakeService(self.socket_path, b"ERR MONITOR_FAILED\n")
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 409)
        self.assertEqual(json.loads(body)["result"], "REJECTED")
        self.assertEqual(fake.commands[-1], b"MUTE\n")
        with mock.patch.object(xs8_gateway, "send_mute") as dispatch:
            self.assertEqual(self.mute(token, cookie)[0], 403)
            dispatch.assert_not_called()

    def test_session_get_reuses_token_and_control_post_rotates_it(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        headers = {"X-Kenwood-Authenticated-User": "remote", "Cookie": cookie}
        code, body, _ = self.request_full("GET", "/api/v1/session", headers=headers)
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)["csrf_token"], token)
        # The service is unavailable, but a validated POST still consumes its token.
        self.assertEqual(self.mute(token, cookie)[0], 503)
        code, body, _ = self.request_full("GET", "/api/v1/session", headers=headers)
        self.assertEqual(code, 200)
        self.assertNotEqual(json.loads(body)["csrf_token"], token)

    def test_fault_status_disables_capability_and_service_refusal_is_preserved(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        fake = FakeService(self.socket_path, STATUS.replace(b"state=IDLE", b"state=FAULT_LATCHED"))
        code, body, _ = self.request_full("GET", "/api/v1/capabilities", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        fake.close()
        self.assertEqual(code, 200)
        self.assertFalse(json.loads(body)["operations"]["mute"]["enabled"])
        fake = OfflineFakeService(self.socket_path, b"ERR MUTE_UNCERTAIN\n")
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 502)
        self.assertEqual(json.loads(body)["result"], "SERVICE_UNCERTAIN")

    def test_session_requires_identity_and_service_availability_disables_mute(self):
        self.enable_dev_mute()
        self.assertEqual(self.request("GET", "/api/v1/session")[0], 401)
        token, cookie = self.session()
        code, body, _ = self.request_full(
            "GET", "/api/v1/capabilities",
            headers={"X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        self.assertEqual(code, 200)
        self.assertFalse(json.loads(body)["operations"]["mute"]["enabled"])
        self.assertTrue(token)

        fake = FakeService(self.socket_path, STATUS)
        code, body, _ = self.request_full(
            "GET", "/api/v1/session",
            headers={"X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        fake.close()
        self.assertEqual(code, 200)
        self.assertTrue(json.loads(body)["mute_available"])
        self.assertEqual(fake.commands, [b"STATUS\n"])


class SourceGatewayTest(GatewayTest):
    def enable_source(self):
        handler = self.server.RequestHandlerClass
        handler.production_source_enabled = True
        handler.https_origin = "https://woodland-pi.local"
        self.server.proxy_peer_verified = True

    def test_all_seven_and_single_dispatch(self):
        self.enable_source()
        for name in xs8_gateway.SOURCES:
            with self.subTest(source=name):
                token, cookie = self.session()
                if name in xs8_gateway.EVIDENCED_SOURCES:
                    fake = RecordingService(self.socket_path,
                        mute_reply=b"OK SOURCE_TX gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED\n")
                    code, body, _ = self.source(token, cookie, name)
                    fake.close()
                    self.assertEqual(code, 200)
                    self.assertEqual(fake.commands.count(f"SELECT_SOURCE {name}\n".encode()), 1)
                    self.assertEqual(json.loads(body)["receiver_execution"], "UNVERIFIED")
                    self.assertEqual(self.source(token, cookie, name)[0], 403)
                else:
                    self.assertEqual(self.source(token, cookie, name)[0], 409)

    def test_authorization_and_invalid_body_never_dispatch(self):
        self.enable_source()
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "send_source") as dispatch:
            self.assertEqual(self.source(token, cookie, "CD", headers={"Origin": "https://evil"})[0], 403)
            self.assertEqual(self.source(token, cookie, "CD", headers={"X-CSRF-Token": "bad"})[0], 403)
            self.assertEqual(self.source(token, cookie, "CD", headers={"Cookie": "bad=1"})[0], 401)
            self.assertEqual(self.source(token, cookie, "CD", headers={"X-Kenwood-Authenticated-User": ""})[0], 401)
            for body in ('{"source":"CD","source":"TUNER"}', '{"source":"BAD"}',
                         '{"source":1}', '{}', '[]', '{"source":"CD","extra":1}'):
                self.assertEqual(self.source(token, cookie, "CD", body=body)[0], 400)
            dispatch.assert_not_called()

    def test_blocked_status_and_uncertain_service(self):
        self.enable_source()
        for state in ("BOOT_INHIBIT", "FAULT_LATCHED", "HOLDING_DOWN"):
            with self.subTest(state=state):
                token, cookie = self.session()
                fake = RecordingService(self.socket_path,
                    status=STATUS.replace(b"state=IDLE", f"state={state}".encode()))
                code, _, _ = self.source(token, cookie, "CD")
                fake.close()
                self.assertEqual(code, 409)
                self.assertNotIn(b"SELECT_SOURCE CD\n", fake.commands)
        token, cookie = self.session()
        fake = RecordingService(self.socket_path,
            mute_reply=b"ERR SOURCE_UNCERTAIN gpio_tx=TX_OK waveform=MISSING receiver_execution=UNVERIFIED\n")
        code, body, _ = self.source(token, cookie, "CD")
        fake.close()
        self.assertEqual(code, 502)
        self.assertEqual(json.loads(body)["result"], "SERVICE_UNCERTAIN")
        self.assertEqual(json.loads(body)["gpio_tx"], "TX_OK")
        self.assertEqual(json.loads(body)["waveform"], "MISSING")
        self.assertEqual(fake.commands.count(b"SELECT_SOURCE CD\n"), 1)

    def test_source_gate_off_and_cli_requires_secure_socket(self):
        self.assertEqual(self.source("x", "x", "CD")[0], 501)
        for options in (("--enable-source",),
                        ("--enable-source", "--systemd-socket", "--nginx-uid", "33"),
                        ("--enable-source", "--systemd-socket", "--nginx-uid", "33",
                         "--https-origin", "https://woodland-pi.local", "--socket", self.socket_path)):
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    xs8_gateway.parse_args(list(options))

    def test_rapid_repeat_is_rejected_server_side(self):
        self.enable_source()
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "read_status", return_value={
                "state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
                "context_may_be_open": "0"}), mock.patch.object(
                    xs8_gateway, "send_source", return_value={
                        "gpio_tx": "TX_OK", "waveform": "MATCH",
                        "receiver_execution": "UNVERIFIED"}) as dispatch:
            self.assertEqual(self.source(token, cookie, "CD")[0], 200)
            code, body, _ = self.request_full("GET", "/api/v1/session", headers={
                "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
            self.assertEqual(code, 200)
            next_token = json.loads(body)["csrf_token"]
            code, body, _ = self.source(next_token, cookie, "CD")
            self.assertEqual(code, 429)
            self.assertEqual(json.loads(body)["result"], "TOO_FAST")
            dispatch.assert_called_once()


class ReceiverQueryGatewayTest(GatewayTest):
    def enable_query(self):
        handler = self.server.RequestHandlerClass
        handler.receiver_query_enabled = True
        handler.https_origin = "https://woodland-pi.local"
        self.server.proxy_peer_verified = True

    def query(self, token, cookie, **overrides):
        headers = {"Origin": "https://woodland-pi.local",
                   "X-Forwarded-Proto": "https",
                   "X-Kenwood-Authenticated-User": "remote",
                   "X-CSRF-Token": token, "Cookie": cookie,
                   "Content-Type": "application/json"}
        headers.update(overrides.pop("headers", {}))
        return self.request_full("POST", "/api/v1/receiver/query",
                                 body=overrides.pop("body", "{}"), headers=headers)

    def test_get_only_reads_status_and_exposes_cache(self):
        fake = RecordingService(self.socket_path, status=RECEIVER_STATUS)
        code, body, _ = self.request_full("GET", "/api/v1/status")
        fake.close()
        self.assertEqual(code, 200)
        state = json.loads(body)["receiver"]
        self.assertEqual(state["source"]["value"], "CD")
        self.assertEqual(state["monitor"]["value"], "ON")
        self.assertEqual(state["source"]["evidence"], "PASSIVE")
        self.assertEqual(fake.commands, [b"STATUS\n"])

    def test_malformed_receiver_fields_are_not_displayed(self):
        bad = RECEIVER_STATUS.replace(b"receiver_source_ns=1000000000",
                                      b"receiver_source_ns=0")
        fake = FakeService(self.socket_path, bad)
        code, _, _ = self.request_full("GET", "/api/v1/status")
        fake.close()
        self.assertEqual(code, 503)

    def test_get_polling_and_capabilities_never_query(self):
        self.enable_query()
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=RECEIVER_STATUS)
        headers = {"X-Kenwood-Authenticated-User": "remote", "Cookie": cookie}
        for _ in range(3):
            self.assertEqual(self.request_full("GET", "/api/v1/status")[0], 200)
            code, body, _ = self.request_full("GET", "/api/v1/capabilities",
                                              headers=headers)
            self.assertEqual(code, 200)
            self.assertTrue(json.loads(body)["operations"]["receiver_query"]["enabled"])
        fake.close()
        self.assertTrue(token)
        self.assertNotIn(b"QUERY_STATUS\n", fake.commands)

    def test_sync_one_dispatch_and_replay(self):
        self.enable_query()
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=RECEIVER_STATUS)
        code, body, _ = self.query(token, cookie)
        fake.close()
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)["result"], "OBSERVATION_RECORDED")
        self.assertEqual(fake.commands.count(b"QUERY_STATUS\n"), 1)
        self.assertEqual(self.query(token, cookie)[0], 403)

    def test_rejected_requests_never_dispatch(self):
        self.enable_query()
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "send_receiver_query") as dispatch:
            self.assertEqual(self.query(token, cookie, headers={"Origin": "https://evil"})[0], 403)
            self.assertEqual(self.query(token, cookie, headers={"X-CSRF-Token": "bad"})[0], 403)
            self.assertEqual(self.query(token, cookie, headers={"Cookie": "bad=1"})[0], 401)
            self.assertEqual(self.query(token, cookie, headers={"Content-Type": "text/plain"})[0], 400)
            self.assertEqual(self.query(token, cookie, body='{"extra":1}')[0], 400)
            dispatch.assert_not_called()

    def test_one_use_token_shared_with_mute_and_source(self):
        self.enable_query()
        handler = self.server.RequestHandlerClass
        handler.production_source_enabled = True
        handler.production_mute_enabled = True
        token, cookie = self.session()
        fake = RecordingService(self.socket_path)
        self.assertEqual(self.query(token, cookie)[0], 200)
        fake.close()
        headers = {"Origin": "https://woodland-pi.local",
                   "X-Forwarded-Proto": "https",
                   "X-Kenwood-Authenticated-User": "remote",
                   "X-CSRF-Token": token, "Cookie": cookie,
                   "Content-Type": "application/json"}
        self.assertEqual(self.request_full("POST", "/api/v1/mute", "{}", headers)[0], 403)
        self.assertEqual(self.request_full("POST", "/api/v1/source",
                                           '{"source":"CD"}', headers)[0], 403)
        self.assertEqual(fake.commands.count(b"QUERY_STATUS\n"), 1)

    def test_inhibited_and_failed_query(self):
        self.enable_query()
        for state in ("BOOT_INHIBIT", "FAULT_LATCHED", "HOLDING_DOWN"):
            token, cookie = self.session()
            fake = RecordingService(self.socket_path,
                status=STATUS.replace(b"state=IDLE", f"state={state}".encode()))
            code, _, _ = self.query(token, cookie)
            fake.close()
            self.assertEqual(code, 409)
            self.assertNotIn(b"QUERY_STATUS\n", fake.commands)
        token, cookie = self.session()
        fake = RecordingService(self.socket_path,
            query_reply=b"ERR QUERY_STATUS reason=TX_UNCERTAIN gpio_tx=TX_OK waveform=MISSING receiver_execution=UNVERIFIED\n")
        code, body, _ = self.query(token, cookie)
        fake.close()
        self.assertEqual(code, 502)
        self.assertEqual(json.loads(body)["result"], "SERVICE_UNCERTAIN")
        self.assertEqual(fake.commands.count(b"QUERY_STATUS\n"), 1)

    def test_gate_off_and_secure_cli(self):
        self.assertEqual(self.query("bad", "bad")[0], 501)
        args = xs8_gateway.parse_args([
            "--enable-receiver-query", "--systemd-socket", "--nginx-uid", "33",
            "--https-origin", "https://woodland-pi.local"])
        self.assertTrue(args.enable_receiver_query)
        for args in (("--enable-receiver-query",),
                     ("--enable-receiver-query", "--systemd-socket", "--nginx-uid", "33"),
                     ("--enable-receiver-query", "--systemd-socket", "--nginx-uid", "33",
                      "--https-origin", "https://woodland-pi.local", "--socket", self.socket_path)):
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    xs8_gateway.parse_args(list(args))


class CdTrackGatewayTest(GatewayTest):
    def enable_cd(self):
        handler = self.server.RequestHandlerClass
        handler.cd_track_enabled = True
        handler.https_origin = "https://woodland-pi.local"
        self.server.proxy_peer_verified = True

    def track(self, token, cookie, action="NEXT", **overrides):
        headers = {"Origin": "https://woodland-pi.local", "X-Forwarded-Proto": "https",
                   "X-Kenwood-Authenticated-User": "remote", "X-CSRF-Token": token,
                   "Cookie": cookie, "Content-Type": "application/json"}
        headers.update(overrides.pop("headers", {}))
        return self.request_full("POST", "/api/v1/cd/track",
                                 body=overrides.pop("body", json.dumps({"action": action})),
                                 headers=headers)

    def dispatched(self, fake):
        return [c for c in fake.commands if c.startswith(b"CD_")]

    def elapse_interval(self):
        # Independent trials: model the server-wide CD interval having passed.
        state = getattr(self.server, "cd_track", None)
        if state:
            state["completed"] = float("-inf")

    def test_gate_off_returns_501_without_service_contact(self):
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS)
        self.assertEqual(self.track("x", "x")[0], 501)
        code, body = self.request("GET", "/api/v1/capabilities")
        fake.close()
        self.assertEqual(fake.commands, [])
        cd = json.loads(body)["operations"]["cd_transport"]
        self.assertFalse(cd["enabled"])
        self.assertEqual(cd["available_actions"], ["NEXT", "PREVIOUS"])
        self.assertIn("STOP", cd["blocked_actions"])
        self.assertIn("PLAY_PAUSE", cd["blocked_actions"])

    def test_next_and_previous_dispatch_once_and_token_is_single_use(self):
        self.enable_cd()
        for action in xs8_gateway.CD_TRACK_ACTIONS:
            with self.subTest(action=action):
                token, cookie = self.session()
                self.elapse_interval()
                fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                        mute_reply=CD_OK.replace(b"NEXT", action.encode()))
                code, body, _ = self.track(token, cookie, action)
                replay = self.track(token, cookie, action)[0]
                fake.close()
                self.assertEqual(code, 200)
                payload = json.loads(body)
                self.assertEqual(payload["result"], "SERVICE_TX_OK")
                self.assertEqual(payload["action"], action)
                self.assertEqual(payload["cd_result"], "TX_VERIFIED")
                self.assertEqual(payload["receiver_execution"], "UNVERIFIED")
                self.assertEqual(self.dispatched(fake), [f"CD_TRACK_{action}\n".encode()])
                self.assertEqual(replay, 403)

    def test_consecutive_actions_use_fresh_tokens_and_dispatch_once_each(self):
        self.enable_cd()
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=CD_OK)
        try:
            self.assertEqual(self.track(token, cookie, "NEXT")[0], 200)
            self.elapse_interval()
            code, body, _ = self.request_full("GET", "/api/v1/session", headers={
                "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
            self.assertEqual(code, 200)
            second = json.loads(body)["csrf_token"]
            self.assertNotEqual(second, token)
            fake.mute_reply = CD_OK.replace(b"NEXT", b"PREVIOUS")
            self.assertEqual(self.track(second, cookie, "PREVIOUS")[0], 200)
            self.assertEqual(self.track(token, cookie, "NEXT")[0], 403)
            self.assertEqual(self.track(second, cookie, "PREVIOUS")[0], 403)
        finally:
            fake.close()
        self.assertEqual(self.dispatched(fake),
                         [b"CD_TRACK_NEXT\n", b"CD_TRACK_PREVIOUS\n"])

    def test_duplicate_submissions_of_one_token_dispatch_once(self):
        self.enable_cd()
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=CD_OK)
        results = []
        threads = [threading.Thread(target=lambda: results.append(
            self.track(token, cookie, "NEXT")[0])) for _ in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=5)
        fake.close()
        self.assertEqual(sorted(results), [200, 403, 403, 403])
        self.assertEqual(self.dispatched(fake), [b"CD_TRACK_NEXT\n"])

    def test_session_and_capabilities_report_cd_availability(self):
        self.enable_cd()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS)
        code, body, headers = self.request_full(
            "GET", "/api/v1/session", headers={"X-Kenwood-Authenticated-User": "remote"})
        self.assertEqual(code, 200)
        session = json.loads(body)
        self.assertTrue(session["cd_track_available"])
        self.assertFalse(session["mute_available"])
        cookie = headers["Set-Cookie"].split(";", 1)[0]
        code, body, _ = self.request_full("GET", "/api/v1/capabilities", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        fake.close()
        self.assertTrue(json.loads(body)["operations"]["cd_transport"]["enabled"])
        self.assertEqual(self.dispatched(fake), [])

    def test_unsupported_actions_and_malformed_bodies_never_dispatch(self):
        self.enable_cd()
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "send_cd_track") as dispatch, \
                mock.patch.object(xs8_gateway, "read_status") as status:
            for body in ('{"action":"STOP"}', '{"action":"PLAY_PAUSE"}',
                         '{"action":"SEEK_FORWARD"}', '{"action":"next"}',
                         '{"action":"NEXT","action":"PREVIOUS"}', '{"action":["NEXT"]}',
                         '{"action":"NEXT","extra":1}', '{}', '[]', '"NEXT"', 'NEXT'):
                with self.subTest(body=body):
                    self.assertEqual(self.track(token, cookie, body=body)[0], 400)
            self.assertEqual(self.track(token, cookie, headers={"Origin": "https://evil"})[0], 403)
            self.assertEqual(self.track(token, cookie, headers={"X-Forwarded-Proto": "http"})[0], 403)
            self.assertEqual(self.track(token, cookie, headers={"X-CSRF-Token": "bad"})[0], 403)
            self.assertEqual(self.track(token, cookie, headers={"Cookie": "bad=1"})[0], 401)
            self.assertEqual(self.track(token, cookie, headers={"X-Kenwood-Authenticated-User": "other"})[0], 401)
            self.assertEqual(self.track(token, cookie, headers={"Content-Type": "text/plain"})[0], 400)
            self.assertEqual(self.track(token, cookie, body=json.dumps({"action": "NEXT" + " " * 60}))[0], 400)
            dispatch.assert_not_called()
            status.assert_not_called()
        self.server.proxy_peer_verified = False
        self.assertEqual(self.track(token, cookie)[0], 501)

    def test_rapid_repeat_is_rejected_server_side_across_actions(self):
        self.enable_cd()
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "send_cd_track", return_value={
                "cd_result": "TX_VERIFIED", "receiver_execution": "UNVERIFIED"}) as dispatch, \
                mock.patch.object(xs8_gateway, "read_status", return_value={
                    "state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
                    "context_may_be_open": "0", "cd_state": "IDLE",
                    "cd_release_owed": "0"}):
            self.assertEqual(self.track(token, cookie, "NEXT")[0], 200)
            code, body, _ = self.request_full("GET", "/api/v1/session", headers={
                "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
            next_token = json.loads(body)["csrf_token"]
            code, body, _ = self.track(next_token, cookie, "PREVIOUS")
            self.assertEqual(code, 429)
            self.assertEqual(json.loads(body)["result"], "TOO_FAST")
            self.assertEqual(self.track(next_token, cookie, "PREVIOUS")[0], 403)
            dispatch.assert_called_once()

    def fresh_token(self, cookie):
        code, body, _ = self.request_full("GET", "/api/v1/session", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        self.assertEqual(code, 200)
        return json.loads(body)["csrf_token"]

    def eligible(self):
        return mock.patch.object(xs8_gateway, "read_status", return_value={
            "state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
            "context_may_be_open": "0", "cd_state": "IDLE", "cd_release_owed": "0"})

    def test_two_rapid_previous_taps_dispatch_separately_after_interval(self):
        self.enable_cd()
        self.server.RequestHandlerClass.cd_track_interval_ms = 200
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                mute_reply=CD_OK.replace(b"NEXT", b"PREVIOUS"))
        try:
            started = time.monotonic()
            self.assertEqual(self.track(token, cookie, "PREVIOUS")[0], 200)
            second = self.fresh_token(cookie)
            self.assertNotEqual(second, token)
            time.sleep(0.25)
            self.assertEqual(self.track(second, cookie, "PREVIOUS")[0], 200)
            elapsed = time.monotonic() - started
            self.assertEqual(self.track(second, cookie, "PREVIOUS")[0], 403)
        finally:
            fake.close()
        # Two separate pairs well inside the old 1 s limit; never merged or repeated.
        self.assertLess(elapsed, 1.0)
        self.assertEqual(self.dispatched(fake),
                         [b"CD_TRACK_PREVIOUS\n", b"CD_TRACK_PREVIOUS\n"])

    def test_interval_is_server_wide_rotates_token_and_reports_interval(self):
        self.enable_cd()
        self.server.RequestHandlerClass.cd_track_interval_ms = 200
        token, cookie = self.session()
        other_token, other_cookie = self.session()
        with mock.patch.object(xs8_gateway, "send_cd_track", return_value={
                "cd_result": "TX_VERIFIED", "receiver_execution": "UNVERIFIED"}) as dispatch, \
                self.eligible():
            self.assertEqual(self.track(token, cookie, "NEXT")[0], 200)
            # A second browser session cannot bypass the changer interval.
            code, body, _ = self.track(other_token, other_cookie, "PREVIOUS")
            self.assertEqual(code, 429)
            self.assertEqual(json.loads(body)["result"], "TOO_FAST")
            self.assertEqual(json.loads(body)["min_interval_ms"], 200)
            self.assertEqual(self.track(other_token, other_cookie, "PREVIOUS")[0], 403)
            time.sleep(0.25)
            self.assertEqual(self.track(self.fresh_token(other_cookie), other_cookie,
                                        "PREVIOUS")[0], 200)
        self.assertEqual([call.args[1] for call in dispatch.call_args_list],
                         ["NEXT", "PREVIOUS"])

    def test_interval_counts_from_completion_including_failed_dispatch(self):
        self.enable_cd()
        self.server.RequestHandlerClass.cd_track_interval_ms = 200

        def slow_timeout(path, action):
            time.sleep(0.3)  # Longer than the interval: request-time spacing would pass.
            raise xs8_gateway.MuteServiceError("TIMEOUT", "no reply")

        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "send_cd_track",
                               side_effect=slow_timeout) as dispatch, self.eligible():
            self.assertEqual(self.track(token, cookie)[0], 504)
            code, body, _ = self.track(self.fresh_token(cookie), cookie)
            self.assertEqual((code, json.loads(body)["result"]), (429, "TOO_FAST"))
            dispatch.assert_called_once()

    def test_refusal_before_dispatch_does_not_start_interval(self):
        self.enable_cd()
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_HOLD_STATUS, mute_reply=CD_OK)
        self.assertEqual(self.track(token, cookie)[0], 409)
        fake.close()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=CD_OK)
        self.assertEqual(self.track(self.fresh_token(cookie), cookie)[0], 200)
        fake.close()
        self.assertEqual(self.dispatched(fake), [b"CD_TRACK_NEXT\n"])

    def test_overlapping_dispatch_is_rejected_without_status_or_dispatch(self):
        self.enable_cd()
        token, cookie = self.session()
        state = self.server.__dict__.setdefault(
            "cd_track", {"lock": threading.Lock(), "completed": float("-inf")})
        with mock.patch.object(xs8_gateway, "send_cd_track", return_value={
                "cd_result": "TX_VERIFIED", "receiver_execution": "UNVERIFIED"}) as dispatch, \
                self.eligible() as status:
            with state["lock"]:  # Another CD request is in flight.
                code, body, _ = self.track(token, cookie)
                self.assertEqual((code, json.loads(body)["result"]), (409, "CD_BUSY"))
                self.assertEqual(self.track(token, cookie)[0], 403)
                status.assert_not_called()
                dispatch.assert_not_called()
            self.assertEqual(self.track(self.fresh_token(cookie), cookie)[0], 200)
            dispatch.assert_called_once()
        self.assertTrue(state["lock"].acquire(blocking=False), "lock released after dispatch")
        state["lock"].release()

    def test_capabilities_report_configured_interval(self):
        self.enable_cd()
        self.server.RequestHandlerClass.cd_track_interval_ms = 400
        _, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS)
        code, body, _ = self.request_full("GET", "/api/v1/capabilities", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        fake.close()
        self.assertEqual(json.loads(body)["operations"]["cd_transport"]["min_interval_ms"], 400)

    @staticmethod
    def server_timing(headers):
        return {name: float(duration.split("=", 1)[1]) for name, duration in (
            item.strip().split(";", 1) for item in headers["Server-Timing"].split(","))}

    def test_server_timing_separates_status_xs8_and_interval(self):
        self.enable_cd()
        self.server.RequestHandlerClass.cd_track_interval_ms = 200
        original = xs8_gateway.read_status

        def slow_status(path, timeout=1.0):
            time.sleep(0.02)
            return original(path, timeout)

        def slow_pair(path, action):
            time.sleep(0.33)  # Code-derived EC/72 pair: two ~110-115 ms frames + 100 ms gap.
            return {"cd_result": "TX_VERIFIED", "receiver_execution": "UNVERIFIED"}

        logged = []
        self.server.RequestHandlerClass.log_message = (
            lambda handler, fmt, *args: logged.append(fmt % args))
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS)
        self.addCleanup(fake.close)
        token, cookie = self.session()
        auth = {"X-Kenwood-Authenticated-User": "remote", "Cookie": cookie}
        with mock.patch.object(xs8_gateway, "read_status", side_effect=slow_status) as status, \
                mock.patch.object(xs8_gateway, "send_cd_track", side_effect=slow_pair):
            code, _, headers = self.track(token, cookie, "PREVIOUS")
            self.assertEqual(code, 200)
            post = self.server_timing(headers)
            self.assertNotIn("sincecd", post)
            self.assertGreaterEqual(post["xs8status"], 20)
            self.assertGreaterEqual(post["xs8cd"], 330)
            self.assertGreaterEqual(post["total"], post["xs8status"] + post["xs8cd"])
            # The browser's token refresh: three sequential GETs, each with a STATUS read.
            refresh = {}
            for path in ("/api/v1/status", "/api/v1/session", "/api/v1/capabilities"):
                code, body, headers = self.request_full("GET", path, headers=auth)
                self.assertEqual(code, 200)
                refresh[path] = self.server_timing(headers)
                self.assertGreaterEqual(refresh[path]["xs8status"], 20)
            second = json.loads(self.request_full(
                "GET", "/api/v1/session", headers=auth)[1])["csrf_token"]
            time.sleep(0.2)  # A fast refresh finishes inside the interval; wait it out.
            code, body, headers = self.track(second, cookie, "PREVIOUS")
            self.assertEqual(code, 200)
            self.assertGreaterEqual(self.server_timing(headers)["sincecd"], 200)
            self.assertEqual(status.call_count, 1 + 3 + 1 + 1)
        lines = [line for line in logged if line.startswith("CD_TRACK ")]
        self.assertEqual(len(lines), 2)
        self.assertRegex(lines[0], r"^CD_TRACK action=PREVIOUS code=200 result=SERVICE_TX_OK "
                                   r"xs8status_ms=\d+\.\d xs8cd_ms=\d+\.\d$")
        self.assertRegex(lines[1], r"sincecd_ms=\d+\.\d xs8status_ms=")

    def test_server_timing_on_refusals_shows_interval_wait(self):
        self.enable_cd()
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "send_cd_track", return_value={
                "cd_result": "TX_VERIFIED", "receiver_execution": "UNVERIFIED"}), \
                self.eligible():
            self.assertEqual(self.track(token, cookie)[0], 200)
            code, _, headers = self.track(self.fresh_token(cookie), cookie)
        self.assertEqual(code, 429)
        timing = self.server_timing(headers)
        self.assertLess(timing["sincecd"], 250)
        self.assertNotIn("xs8cd", timing)
        self.assertNotIn("xs8status", timing)

    def test_zero_interval_removes_cooldown_but_keeps_every_other_check(self):
        self.enable_cd()
        self.server.RequestHandlerClass.cd_track_interval_ms = 0
        token, cookie = self.session()
        state = self.server.__dict__.setdefault(
            "cd_track", {"lock": threading.Lock(), "completed": float("-inf")})
        with mock.patch.object(xs8_gateway, "send_cd_track", return_value={
                "cd_result": "TX_VERIFIED", "receiver_execution": "UNVERIFIED"}) as dispatch, \
                self.eligible() as status:
            self.assertEqual(self.track(token, cookie, "PREVIOUS")[0], 200)
            second = self.fresh_token(cookie)
            code, _, headers = self.track(second, cookie, "PREVIOUS")
            self.assertEqual(code, 200, "no TOO_FAST at zero cooldown")
            timing = self.server_timing(headers)
            self.assertIn("sincecd", timing)
            self.assertLess(timing["sincecd"], 200)
            self.assertEqual(self.track(second, cookie, "PREVIOUS")[0], 403, "token still single-use")
            with state["lock"]:  # Serialization still applies.
                code, body, _ = self.track(self.fresh_token(cookie), cookie, "PREVIOUS")
                self.assertEqual((code, json.loads(body)["result"]), (409, "CD_BUSY"))
            self.assertEqual(dispatch.call_count, 2)
            # Every dispatched command still had its own fresh STATUS read.
            posts_with_status = status.call_count
        self.assertGreaterEqual(posts_with_status, 2)
        _, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS)
        code, body, _ = self.request_full("GET", "/api/v1/capabilities", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        fake.close()
        self.assertIs(json.loads(body)["operations"]["cd_transport"]["min_interval_ms"], 0)

    def test_zero_interval_still_rejects_ineligible_status_and_bad_replies(self):
        self.enable_cd()
        self.server.RequestHandlerClass.cd_track_interval_ms = 0
        for status, reply, code, result in (
                (CD_HOLD_STATUS, CD_OK, 409, "CD_BUSY"),
                (CD_FAULT_STATUS, CD_OK, 409, "CD_FAULT_LATCHED"),
                (CD_IDLE_STATUS, CD_OK.replace(b"action=NEXT", b"action=PREVIOUS"), 502,
                 "INVALID_REPLY"),
                (CD_IDLE_STATUS, CD_UNCERTAIN, 502, "SERVICE_UNCERTAIN")):
            with self.subTest(result=result):
                token, cookie = self.session()
                fake = RecordingService(self.socket_path, status=status, mute_reply=reply)
                got, body, _ = self.track(token, cookie)
                fake.close()
                self.assertEqual((got, json.loads(body)["result"]), (code, result))
                self.assertLessEqual(len(self.dispatched(fake)), 1, "never retried")

    def test_success_returns_rotated_single_use_token(self):
        self.enable_cd()
        self.server.RequestHandlerClass.cd_track_interval_ms = 0
        logged = []
        self.server.RequestHandlerClass.log_message = (
            lambda handler, fmt, *args: logged.append(fmt % args))
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                mute_reply=CD_OK.replace(b"NEXT", b"PREVIOUS"))
        try:
            code, body, headers = self.track(token, cookie, "PREVIOUS")
            self.assertEqual(code, 200)
            returned = json.loads(body)["csrf_token"]
            self.assertNotEqual(returned, token)
            self.assertEqual(returned, self.fresh_token(cookie), "same as the session token")
            self.assertNotIn(returned, json.dumps(dict(headers)))
            self.assertEqual(self.track(token, cookie, "PREVIOUS")[0], 403, "old token replay")
            code, body, _ = self.track(returned, cookie, "PREVIOUS")
            self.assertEqual(code, 200, "returned token authorizes exactly one more POST")
            third = json.loads(body)["csrf_token"]
            self.assertEqual(self.track(returned, cookie, "PREVIOUS")[0], 403)
            self.assertNotIn(third, (token, returned))
        finally:
            fake.close()
        self.assertEqual(self.dispatched(fake), [b"CD_TRACK_PREVIOUS\n"] * 2)
        # Every CD request still read STATUS before its dispatch.
        commands = [c for c in fake.commands if c in (b"STATUS\n", b"CD_TRACK_PREVIOUS\n")]
        for index, command in enumerate(commands):
            if command == b"CD_TRACK_PREVIOUS\n":
                self.assertEqual(commands[index - 1], b"STATUS\n")
        log = "\n".join(logged)
        for value in (token, returned, third):
            self.assertNotIn(value, log, "tokens never reach the journal")

    def test_failures_never_return_a_token(self):
        self.enable_cd()
        self.server.RequestHandlerClass.cd_track_interval_ms = 0
        cases = [(CD_HOLD_STATUS, CD_OK), (CD_FAULT_STATUS, CD_OK),
                 (CD_IDLE_STATUS, CD_UNCERTAIN), (CD_IDLE_STATUS, b"ERR CD_TRACK_BUSY\n"),
                 (CD_IDLE_STATUS, CD_OK.replace(b"action=NEXT", b"action=PREVIOUS")),
                 (CD_IDLE_STATUS, b"ERR CD_FAULT_LATCHED\n")]
        for status, reply in cases:
            with self.subTest(reply=reply, status=status[:20]):
                token, cookie = self.session()
                fake = RecordingService(self.socket_path, status=status, mute_reply=reply)
                code, body, _ = self.track(token, cookie)
                fake.close()
                self.assertNotEqual(code, 200)
                self.assertNotIn("csrf_token", json.loads(body))
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "send_cd_track", side_effect=xs8_gateway.MuteServiceError(
                "TIMEOUT", "no reply")), self.eligible():
            code, body, _ = self.track(token, cookie)
        self.assertEqual(code, 504)
        self.assertNotIn("csrf_token", json.loads(body))
        state = self.server.__dict__["cd_track"]
        with state["lock"]:
            code, body, _ = self.track(self.fresh_token(cookie), cookie)
        self.assertEqual(code, 409)
        self.assertNotIn("csrf_token", json.loads(body))
        self.server.RequestHandlerClass.cd_track_interval_ms = 1000
        with mock.patch.object(xs8_gateway, "send_cd_track", return_value={
                "cd_result": "TX_VERIFIED", "receiver_execution": "UNVERIFIED"}), self.eligible():
            code, body, _ = self.track(self.fresh_token(cookie), cookie)
        self.assertEqual((code, json.loads(body)["result"]), (429, "TOO_FAST"))
        self.assertNotIn("csrf_token", json.loads(body))

    def test_ineligible_status_never_dispatches(self):
        self.enable_cd()
        cases = {
            "boot_inhibit": (CD_IDLE_STATUS.replace(b"OK state=IDLE", b"OK state=BOOT_INHIBIT"), "NOT_ELIGIBLE"),
            "volume_held": (CD_IDLE_STATUS.replace(b"OK state=IDLE", b"OK state=HOLDING_UP"), "NOT_ELIGIBLE"),
            "stop_owed": (CD_IDLE_STATUS.replace(b"stop_owed=0", b"stop_owed=1"), "NOT_ELIGIBLE"),
            "context": (CD_IDLE_STATUS.replace(b"context_may_be_open=0", b"context_may_be_open=1"), "NOT_ELIGIBLE"),
            "monitor": (CD_IDLE_STATUS.replace(b"monitor=HEALTHY", b"monitor=FAILED"), "NOT_ELIGIBLE"),
            "cd_hold": (CD_HOLD_STATUS, "CD_BUSY"),
            "idle_release_owed": (CD_IDLE_STATUS.replace(b"cd_release_owed=0",
                                                         b"cd_release_owed=1"), "CD_FAULT_LATCHED"),
            "no_cd_fields": (STATUS, "NOT_ELIGIBLE"),
            "cd_fault": (CD_FAULT_STATUS, "CD_FAULT_LATCHED"),
        }
        for name, (status, result) in cases.items():
            with self.subTest(case=name):
                token, cookie = self.session()
                fake = RecordingService(self.socket_path, status=status, mute_reply=CD_OK)
                code, body, _ = self.track(token, cookie)
                fake.close()
                self.assertEqual(code, 409)
                self.assertEqual(json.loads(body)["result"], result)
                self.assertEqual(self.dispatched(fake), [])

    def test_uncertain_reply_reports_evidence_and_requires_physical_check(self):
        self.enable_cd()
        for reply, cd_result in ((CD_UNCERTAIN, "RELEASE_UNCERTAIN"),
                                 (CD_UNCERTAIN.replace(b"RELEASE_UNCERTAIN", b"PRESS_UNCERTAIN"),
                                  "PRESS_UNCERTAIN")):
            with self.subTest(cd_result=cd_result):
                token, cookie = self.session()
                self.elapse_interval()
                fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                        mute_reply=reply)
                code, body, _ = self.track(token, cookie)
                fake.close()
                self.assertEqual(code, 502)
                payload = json.loads(body)
                self.assertEqual(payload["result"], "SERVICE_UNCERTAIN")
                self.assertEqual(payload["cd_result"], cd_result)
                self.assertEqual(payload["release_waveform"], "MISSING")
                self.assertTrue(payload["physical_check_required"])
                self.assertEqual(self.dispatched(fake), [b"CD_TRACK_NEXT\n"])

    def test_service_refusals_and_fault_reply(self):
        self.enable_cd()
        for reply, code, result, check in (
                (b"ERR CD_FAULT_LATCHED\n", 409, "CD_FAULT_LATCHED", True),
                (b"ERR CD_TRACK_BUSY\n", 409, "REJECTED", False),
                (b"ERR RESEARCH reason=SESSION_BUSY\n", 409, "REJECTED", False)):
            with self.subTest(reply=reply):
                token, cookie = self.session()
                self.elapse_interval()
                fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                        mute_reply=reply)
                status, body, _ = self.track(token, cookie)
                fake.close()
                self.assertEqual(status, code)
                payload = json.loads(body)
                self.assertEqual(payload["result"], result)
                self.assertIs(payload["physical_check_required"], check)
                self.assertEqual(len(self.dispatched(fake)), 1)

    def test_malformed_tx_reports_are_uncertain_not_refusals(self):
        self.enable_cd()
        replies = (
            CD_OK.replace(b"action=NEXT", b"action=PREVIOUS"),
            CD_OK.replace(b"release_waveform=MATCH", b"release_waveform=MISSING"),
            CD_OK.replace(b"cd_result=TX_VERIFIED", b"cd_result=PRESS_UNCERTAIN"),
            CD_OK.replace(b" receiver_execution=UNVERIFIED", b""),
            CD_OK.replace(b"receiver_execution=UNVERIFIED", b"receiver_execution=VERIFIED"),
            CD_UNCERTAIN.replace(b"cd_result=RELEASE_UNCERTAIN", b"cd_result=TX_VERIFIED"),
            CD_UNCERTAIN.replace(b"press_waveform=MATCH ", b""),
            b"OK MUTE_TX\n", b"OK\n", b"\n")
        for reply in replies:
            with self.subTest(reply=reply):
                token, cookie = self.session()
                self.elapse_interval()
                fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                        mute_reply=reply)
                code, body, _ = self.track(token, cookie)
                fake.close()
                self.assertEqual(code, 502)
                payload = json.loads(body)
                self.assertEqual(payload["result"], "INVALID_REPLY")
                self.assertTrue(payload["physical_check_required"])
                self.assertEqual(len(self.dispatched(fake)), 1)

    def test_timeout_and_disconnect_are_uncertain_without_retry(self):
        self.enable_cd()
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "read_status", return_value={
                "state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
                "context_may_be_open": "0", "cd_state": "IDLE",
                "cd_release_owed": "0"}):
            fake = FakeService(self.socket_path, CD_OK, delay=0.3)
            original = xs8_gateway.send_cd_track
            with mock.patch.object(xs8_gateway, "send_cd_track",
                                   side_effect=lambda path, action: original(
                                       path, action, timeout=0.1)) as dispatch:
                code, body, _ = self.track(token, cookie)
            fake.close()
            self.assertEqual(code, 504)
            self.assertEqual(json.loads(body)["result"], "TIMEOUT")
            self.assertTrue(json.loads(body)["physical_check_required"])
            dispatch.assert_called_once()
            self.assertEqual(fake.commands, [b"CD_TRACK_NEXT\n"])
            token, cookie = self.session()
            self.elapse_interval()
            fake = FakeService(self.socket_path, None)
            code, body, _ = self.track(token, cookie)
            fake.close()
            self.assertEqual(code, 502)
            self.assertEqual(json.loads(body)["result"], "DISCONNECTED")
            self.assertTrue(json.loads(body)["physical_check_required"])
            self.assertEqual(fake.commands, [b"CD_TRACK_NEXT\n"])
        token, cookie = self.session()
        self.elapse_interval()
        code, body, _ = self.track(token, cookie)
        self.assertEqual(code, 503)
        self.assertEqual(json.loads(body)["result"], "UNAVAILABLE")

    def test_default_timeout_covers_discrete_pair(self):
        with mock.patch.object(xs8_gateway, "_send_control",
                               return_value=CD_OK.decode().strip()) as control:
            xs8_gateway.send_cd_track("/unused", "NEXT")
        self.assertEqual(control.call_args.args[1], b"CD_TRACK_NEXT\n")
        self.assertGreaterEqual(control.call_args.args[2], 3.0)
        self.assertFalse(control.call_args.kwargs["fake_only"])
        with self.assertRaises(xs8_gateway.MuteServiceError):
            xs8_gateway.send_cd_track("/unused", "STOP")

    def test_cli_gate_requires_secure_socket(self):
        args = xs8_gateway.parse_args([
            "--enable-cd-track", "--systemd-socket", "--nginx-uid", "33",
            "--https-origin", "https://woodland-pi.local"])
        self.assertTrue(args.enable_cd_track)
        self.assertFalse(xs8_gateway.parse_args([]).enable_cd_track)
        self.assertEqual(args.cd_track_interval_ms, 250)
        base = ["--enable-cd-track", "--systemd-socket", "--nginx-uid", "33",
                "--https-origin", "https://woodland-pi.local", "--cd-track-interval-ms"]
        for value in ("0", "200", "1000"):
            self.assertEqual(xs8_gateway.parse_args(base + [value]).cd_track_interval_ms,
                             int(value))
        for value in ("1", "50", "199", "1001", "-250", "x"):
            with self.subTest(interval=value):
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        xs8_gateway.parse_args(base + [value])
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                xs8_gateway.parse_args(["--cd-track-interval-ms", "250"])
        for options in (("--enable-cd-track",),
                        ("--enable-cd-track", "--systemd-socket", "--nginx-uid", "33"),
                        ("--enable-cd-track", "--systemd-socket", "--nginx-uid", "33",
                         "--https-origin", "https://woodland-pi.local", "--socket", self.socket_path),
                        ("--enable-cd-track", "--enable-offline-mute", "--systemd-socket",
                         "--nginx-uid", "33", "--https-origin", "https://woodland-pi.local",
                         "--socket", self.socket_path)):
            with self.subTest(options=options):
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        xs8_gateway.parse_args(list(options))


class VolumeStepGatewayTest(GatewayTest):
    def enable_volume(self):
        handler = self.server.RequestHandlerClass
        handler.volume_step_enabled = True
        handler.https_origin = "https://woodland-pi.local"
        self.server.proxy_peer_verified = True

    def step(self, token, cookie, direction="UP", **overrides):
        headers = {"Origin": "https://woodland-pi.local", "X-Forwarded-Proto": "https",
                   "X-Kenwood-Authenticated-User": "remote", "X-CSRF-Token": token,
                   "Cookie": cookie, "Content-Type": "application/json"}
        headers.update(overrides.pop("headers", {}))
        return self.request_full("POST", "/api/v1/volume/step",
                                 body=overrides.pop("body", json.dumps({"direction": direction})),
                                 headers=headers)

    def dispatched(self, fake):
        return [c for c in fake.commands if c.startswith(b"VOLUME_")]

    def elapse_interval(self):
        state = getattr(self.server, "volume_step", None)
        if state:
            state["completed"] = float("-inf")

    def fresh_token(self, cookie):
        code, body, _ = self.request_full("GET", "/api/v1/session", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        self.assertEqual(code, 200)
        return json.loads(body)["csrf_token"]

    def eligible(self):
        return mock.patch.object(xs8_gateway, "read_status", return_value={
            "state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
            "context_may_be_open": "0", "cd_state": "IDLE", "cd_release_owed": "0"})

    def test_gate_off_returns_501_and_capability_disabled(self):
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=VOLUME_OK)
        self.assertEqual(self.step("x", "x")[0], 501)
        # Other gates do not open this one.
        self.server.RequestHandlerClass.cd_track_enabled = True
        self.server.RequestHandlerClass.https_origin = "https://woodland-pi.local"
        self.server.proxy_peer_verified = True
        token, cookie = self.session()
        self.assertEqual(self.step(token, cookie)[0], 501)
        code, body, _ = self.request_full("GET", "/api/v1/session", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        self.assertFalse(json.loads(body)["volume_step_available"])
        code, body, _ = self.request_full("GET", "/api/v1/capabilities", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        fake.close()
        volume = json.loads(body)["operations"]["volume_step"]
        self.assertFalse(volume["enabled"])
        self.assertEqual(volume["available_directions"], ["UP", "DOWN"])
        self.assertEqual(volume["min_interval_ms"], 250)
        self.assertEqual(self.dispatched(fake), [])

    def test_session_and_capabilities_report_availability(self):
        self.enable_volume()
        for status, available in ((CD_IDLE_STATUS, True), (STATUS, False),
                                  (CD_IDLE_STATUS.replace(b"OK state=IDLE",
                                                          b"OK state=BOOT_INHIBIT"), False),
                                  (CD_HOLD_STATUS, False), (CD_FAULT_STATUS, False)):
            with self.subTest(status=status[-40:]):
                fake = RecordingService(self.socket_path, status=status, mute_reply=VOLUME_OK)
                code, body, headers = self.request_full(
                    "GET", "/api/v1/session",
                    headers={"X-Kenwood-Authenticated-User": "remote"})
                self.assertEqual(code, 200)
                session = json.loads(body)
                self.assertIs(session["volume_step_available"], available)
                self.assertFalse(session["cd_track_available"])
                cookie = headers["Set-Cookie"].split(";", 1)[0]
                code, body, _ = self.request_full("GET", "/api/v1/capabilities", headers={
                    "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
                # Without a session, capabilities never report an enabled control.
                anonymous = json.loads(self.request("GET", "/api/v1/capabilities")[1])
                fake.close()
                volume = json.loads(body)["operations"]["volume_step"]
                self.assertIs(volume["enabled"], available)
                self.assertEqual(volume["classification"], "unverified")
                self.assertFalse(anonymous["operations"]["volume_step"]["enabled"])
                self.assertEqual(self.dispatched(fake), [])

    def test_up_and_down_dispatch_once_and_token_is_single_use(self):
        self.enable_volume()
        for direction in ("UP", "DOWN"):
            with self.subTest(direction=direction):
                token, cookie = self.session()
                self.elapse_interval()
                fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                        mute_reply=VOLUME_OK.replace(b"UP", direction.encode()))
                code, body, _ = self.step(token, cookie, direction)
                replay = self.step(token, cookie, direction)[0]
                fake.close()
                self.assertEqual(code, 200)
                payload = json.loads(body)
                self.assertEqual(payload["result"], "SERVICE_TX_OK")
                self.assertEqual(payload["direction"], direction)
                self.assertEqual(payload["volume_result"], "TX_VERIFIED")
                self.assertEqual(payload["receiver_execution"], "UNVERIFIED")
                self.assertEqual(self.dispatched(fake), [f"VOLUME_STEP {direction}\n".encode()])
                # STATUS preflight precedes the single dispatch.
                self.assertEqual(fake.commands, [b"STATUS\n",
                                                 f"VOLUME_STEP {direction}\n".encode()])
                self.assertEqual(replay, 403)

    def test_success_returns_single_use_successor_token_never_logged(self):
        self.enable_volume()
        logged = []
        self.server.RequestHandlerClass.log_message = (
            lambda handler, fmt, *args: logged.append(fmt % args))
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=VOLUME_OK)
        try:
            code, body, headers = self.step(token, cookie)
            self.assertEqual(code, 200)
            returned = json.loads(body)["csrf_token"]
            self.assertNotEqual(returned, token)
            self.assertEqual(returned, self.fresh_token(cookie))
            self.assertNotIn(returned, json.dumps(dict(headers)))
            self.elapse_interval()
            self.assertEqual(self.step(returned, cookie)[0], 200)
            self.elapse_interval()
            self.assertEqual(self.step(returned, cookie)[0], 403)
        finally:
            fake.close()
        self.assertEqual(self.dispatched(fake), [b"VOLUME_STEP UP\n"] * 2)
        log = "\n".join(logged)
        self.assertIn("VOLUME_STEP direction=UP code=200 result=SERVICE_TX_OK", log)
        self.assertIn("xs8volume_ms=", log)
        self.assertNotIn(token, log)
        self.assertNotIn(returned, log)

    def test_invalid_directions_and_bodies_never_dispatch(self):
        self.enable_volume()
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=VOLUME_OK)
        bodies = ('{"direction":"up"}', '{"direction":"LEFT"}', '{"direction":""}',
                  '{"direction":null}', '{"direction":1}', '{"direction":["UP"]}',
                  '{}', '{"direction":"UP","extra":1}',
                  '{"direction":"UP","direction":"UP"}', '{"action":"UP"}',
                  '["UP"]', '"UP"', 'UP', '{"direction":"UP"', '')
        try:
            for body in bodies:
                with self.subTest(body=body):
                    code, _, _ = self.step(token, cookie, body=body)
                    self.assertEqual(code, 400)
                    token = self.fresh_token(cookie)
            code, _, _ = self.step(token, cookie, body=json.dumps({"direction": "U" * 80}))
            self.assertEqual(code, 400)
        finally:
            fake.close()
        self.assertEqual(self.dispatched(fake), [])

    def test_authentication_csrf_and_origin_never_dispatch(self):
        self.enable_volume()
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=VOLUME_OK)
        try:
            cases = (
                ({"X-CSRF-Token": "wrong"}, 403),
                ({"Cookie": "xs8_session=missing"}, 401),
                ({"X-Kenwood-Authenticated-User": "other"}, 401),
                ({"Origin": "https://evil.example"}, 403),
                ({"X-Forwarded-Proto": "http"}, 403),
                ({"Content-Type": "text/plain"}, 400),
            )
            for headers, expected in cases:
                with self.subTest(headers=headers):
                    self.assertEqual(self.step(token, cookie, headers=headers)[0], expected)
            # Without a verified proxy peer the endpoint stays inert.
            self.server.proxy_peer_verified = False
            self.assertEqual(self.step(token, cookie)[0], 501)
            self.server.proxy_peer_verified = True
            code, _, _ = self.step(self.fresh_token(cookie), cookie)
            self.assertEqual(code, 200)
        finally:
            fake.close()
        self.assertEqual(self.dispatched(fake), [b"VOLUME_STEP UP\n"])

    def test_status_gating_never_dispatches(self):
        self.enable_volume()
        cases = {
            "boot_inhibit": (CD_IDLE_STATUS.replace(b"OK state=IDLE", b"OK state=BOOT_INHIBIT"),
                             "NOT_ELIGIBLE", None),
            "cec_hold": (CD_IDLE_STATUS.replace(b"OK state=IDLE", b"OK state=HOLDING_UP"),
                         "VOLUME_BUSY", None),
            "stop_owed": (CD_IDLE_STATUS.replace(b"stop_owed=0", b"stop_owed=1"),
                          "NOT_ELIGIBLE", None),
            "context": (CD_IDLE_STATUS.replace(b"context_may_be_open=0",
                                               b"context_may_be_open=1"), "NOT_ELIGIBLE", None),
            "monitor": (CD_IDLE_STATUS.replace(b"monitor=HEALTHY", b"monitor=FAILED"),
                        "NOT_ELIGIBLE", None),
            "cd_hold": (CD_HOLD_STATUS, "CD_BUSY", None),
            "cd_fault": (CD_FAULT_STATUS, "CD_FAULT_LATCHED", True),
            "volume_fault": (CD_IDLE_STATUS.replace(b"OK state=IDLE", b"OK state=FAULT_LATCHED"),
                             "FAULT_LATCHED", True),
            "no_cd_fields": (STATUS, "NOT_ELIGIBLE", None),
        }
        for name, (status, result, check) in cases.items():
            with self.subTest(case=name):
                token, cookie = self.session()
                fake = RecordingService(self.socket_path, status=status, mute_reply=VOLUME_OK)
                code, body, _ = self.step(token, cookie)
                replay = self.step(token, cookie)[0]
                fake.close()
                self.assertEqual(code, 409)
                payload = json.loads(body)
                self.assertEqual(payload["result"], result)
                self.assertEqual(payload["receiver_execution"], "UNVERIFIED")
                self.assertIs(payload.get("physical_check_required"), check)
                self.assertNotIn("csrf_token", payload)
                self.assertEqual(replay, 403, "the token was consumed")
                self.assertEqual(self.dispatched(fake), [])
        token, cookie = self.session()
        code, body, _ = self.step(token, cookie)
        self.assertEqual((code, json.loads(body)["result"]), (503, "UNAVAILABLE"))

    def test_service_replies_are_classified_and_never_retried(self):
        self.enable_volume()
        cases = (
            (VOLUME_UNCERTAIN, 502, "SERVICE_UNCERTAIN", True),
            (VOLUME_UNCERTAIN.replace(b"UNCERTAIN direction", b"CANCELLED direction")
             .replace(b"FAULT_LATCHED", b"IDLE").replace(b"=1", b"=0"),
             502, "SERVICE_UNCERTAIN", True),
            (b"ERR FAULT_LATCHED\n", 409, "FAULT_LATCHED", True),
            (b"ERR CD_FAULT_LATCHED\n", 409, "CD_FAULT_LATCHED", True),
            (b"ERR VOLUME_BUSY\n", 409, "REJECTED", False),
            (b"ERR CD_TRACK_BUSY\n", 409, "REJECTED", False),
            (b"ERR UNKNOWN_COMMAND\n", 409, "REJECTED", False),
            (b"ERR SHUTDOWN\n", 409, "REJECTED", False),
        )
        for reply, code, result, check in cases:
            with self.subTest(reply=reply):
                token, cookie = self.session()
                self.elapse_interval()
                fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                        mute_reply=reply)
                status, body, _ = self.step(token, cookie)
                fake.close()
                self.assertEqual(status, code)
                payload = json.loads(body)
                self.assertEqual(payload["result"], result)
                self.assertEqual(payload["direction"], "UP")
                self.assertIs(payload["physical_check_required"], check)
                self.assertEqual(payload["receiver_execution"], "UNVERIFIED")
                self.assertNotIn("csrf_token", payload)
                self.assertEqual(self.dispatched(fake), [b"VOLUME_STEP UP\n"])
        token, cookie = self.session()
        self.elapse_interval()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                mute_reply=VOLUME_UNCERTAIN)
        _, body, _ = self.step(token, cookie)
        fake.close()
        payload = json.loads(body)
        self.assertEqual((payload["volume_result"], payload["state"], payload["stop_owed"],
                          payload["context_may_be_open"]),
                         ("UNCERTAIN", "FAULT_LATCHED", "1", "1"))

    def test_malformed_tx_reports_are_uncertain_not_refusals(self):
        self.enable_volume()
        replies = (
            VOLUME_OK.replace(b"direction=UP", b"direction=DOWN"),
            VOLUME_OK.replace(b"stop_owed=0", b"stop_owed=1"),
            VOLUME_OK.replace(b"context_may_be_open=0", b"context_may_be_open=1"),
            VOLUME_OK.replace(b"state=IDLE", b"state=FAULT_LATCHED"),
            VOLUME_OK.replace(b" receiver_execution=UNVERIFIED", b""),
            VOLUME_OK.replace(b"receiver_execution=UNVERIFIED", b"receiver_execution=VERIFIED"),
            VOLUME_OK.replace(b"\n", b" extra=1\n"),
            VOLUME_UNCERTAIN.replace(b"stop_owed=1", b"stop_owed=2"),
            VOLUME_UNCERTAIN.replace(b" context_may_be_open=1", b""),
            b"ERR VOLUME_STEP_UNCERTAIN\n", b"OK MUTE_TX\n", b"OK\n", b"\n",
            CD_OK)
        for reply in replies:
            with self.subTest(reply=reply):
                token, cookie = self.session()
                self.elapse_interval()
                fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                        mute_reply=reply)
                code, body, _ = self.step(token, cookie)
                fake.close()
                self.assertEqual(code, 502)
                payload = json.loads(body)
                self.assertEqual(payload["result"], "INVALID_REPLY")
                self.assertTrue(payload["physical_check_required"])
                self.assertEqual(self.dispatched(fake), [b"VOLUME_STEP UP\n"])

    def test_timeout_and_disconnect_are_uncertain_without_retry(self):
        self.enable_volume()
        token, cookie = self.session()
        with self.eligible():
            fake = FakeService(self.socket_path, VOLUME_OK, delay=0.3)
            original = xs8_gateway.send_volume_step
            with mock.patch.object(xs8_gateway, "send_volume_step",
                                   side_effect=lambda path, direction: original(
                                       path, direction, timeout=0.1)) as dispatch:
                code, body, _ = self.step(token, cookie)
            fake.close()
            self.assertEqual(code, 504)
            payload = json.loads(body)
            self.assertEqual(payload["result"], "TIMEOUT")
            self.assertTrue(payload["physical_check_required"])
            self.assertNotIn("csrf_token", payload)
            dispatch.assert_called_once()
            self.assertEqual(fake.commands, [b"VOLUME_STEP UP\n"])
            token = self.fresh_token(cookie)
            self.elapse_interval()
            fake = FakeService(self.socket_path, None)
            code, body, _ = self.step(token, cookie)
            fake.close()
            self.assertEqual(code, 502)
            self.assertEqual(json.loads(body)["result"], "DISCONNECTED")
            self.assertTrue(json.loads(body)["physical_check_required"])
            self.assertEqual(fake.commands, [b"VOLUME_STEP UP\n"])
            token = self.fresh_token(cookie)
            self.elapse_interval()
            code, body, _ = self.step(token, cookie)
            self.assertEqual(code, 503)
            self.assertEqual(json.loads(body)["result"], "UNAVAILABLE")
            self.assertFalse(json.loads(body)["physical_check_required"])

    def test_duplicate_submissions_of_one_token_dispatch_once(self):
        self.enable_volume()
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=VOLUME_OK)
        results = []
        threads = [threading.Thread(target=lambda: results.append(
            self.step(token, cookie)[0])) for _ in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=5)
        fake.close()
        self.assertEqual(sorted(results), [200, 403, 403, 403])
        self.assertEqual(self.dispatched(fake), [b"VOLUME_STEP UP\n"])

    def test_browser_disconnect_after_post_dispatches_once_without_retry(self):
        self.enable_volume()
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=VOLUME_OK)
        try:
            body = json.dumps({"direction": "DOWN"})
            request = (f"POST /api/v1/volume/step HTTP/1.1\r\nHost: x\r\n"
                       f"Origin: https://woodland-pi.local\r\nX-Forwarded-Proto: https\r\n"
                       f"X-Kenwood-Authenticated-User: remote\r\nX-CSRF-Token: {token}\r\n"
                       f"Cookie: {cookie}\r\nContent-Type: application/json\r\n"
                       f"Content-Length: {len(body)}\r\n\r\n{body}")
            with socket.create_connection(("127.0.0.1", self.server.server_port)) as client:
                client.sendall(request.encode())
            # The Gateway still serves the next request; the abandoned one is not replayed.
            deadline = time.monotonic() + 2
            while not self.dispatched(fake) and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertEqual(self.step(token, cookie)[0], 403)
            self.elapse_interval()
            self.assertEqual(self.step(self.fresh_token(cookie), cookie, "UP")[0], 200)
        finally:
            fake.close()
        self.assertEqual(self.dispatched(fake), [b"VOLUME_STEP DOWN\n", b"VOLUME_STEP UP\n"])

    def test_overlap_and_interval_are_server_wide_and_rotate_token(self):
        self.enable_volume()
        token, cookie = self.session()
        state = self.server.__dict__.setdefault(
            "volume_step", {"lock": threading.Lock(), "completed": float("-inf")})
        with mock.patch.object(xs8_gateway, "send_volume_step") as dispatch, self.eligible():
            with state["lock"]:
                code, body, _ = self.step(token, cookie)
            self.assertEqual((code, json.loads(body)["result"]), (409, "VOLUME_BUSY"))
            self.assertEqual(self.step(token, cookie)[0], 403, "busy refusal consumed the token")
            state["completed"] = time.monotonic()
            code, body, _ = self.step(self.fresh_token(cookie), cookie, "DOWN")
            payload = json.loads(body)
            self.assertEqual((code, payload["result"]), (429, "TOO_FAST"))
            self.assertEqual(payload["min_interval_ms"], 250)
            self.assertNotIn("csrf_token", payload)
            dispatch.assert_not_called()

    def test_interval_counts_from_completion_including_failed_dispatch(self):
        self.enable_volume()
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway, "send_volume_step", side_effect=(
                xs8_gateway.MuteServiceError("TIMEOUT", "no reply"))) as dispatch, \
                self.eligible():
            self.assertEqual(self.step(token, cookie)[0], 504)
            self.assertEqual(self.step(self.fresh_token(cookie), cookie)[0], 429)
            self.assertEqual(dispatch.call_count, 1)
        # CD spacing is separate from volume spacing.
        self.assertNotIn("cd_track", self.server.__dict__)

    def test_every_failure_leaves_a_recoverable_token_without_dispatch(self):
        """Each failure consumes the POSTed token; GET /api/v1/session supplies its
        successor and never dispatches. The successor authorizes exactly one step."""
        self.enable_volume()
        state = self.server.__dict__.setdefault(
            "volume_step", {"lock": threading.Lock(), "completed": float("-inf")})
        hold_status = CD_IDLE_STATUS.replace(b"OK state=IDLE", b"OK state=HOLDING_UP")
        cases = (
            ("VOLUME_BUSY lock", 409, None, None),
            ("VOLUME_BUSY hold", 409, hold_status, VOLUME_OK),
            ("REJECTED", 409, CD_IDLE_STATUS, b"ERR VOLUME_BUSY\n"),
            ("SERVICE_UNCERTAIN", 502, CD_IDLE_STATUS, VOLUME_UNCERTAIN),
            ("INVALID_REPLY", 502, CD_IDLE_STATUS, b"OK\n"),
            ("TIMEOUT", 504, None, None),
            ("DISCONNECTED", 502, CD_IDLE_STATUS, None),
            ("TOO_FAST", 429, None, None),
        )
        for name, code, status, reply in cases:
            with self.subTest(case=name):
                token, cookie = self.session()
                self.elapse_interval()
                if status is not None:
                    fake = RecordingService(self.socket_path, status=status, mute_reply=reply)
                    try:
                        self.assertEqual(self.step(token, cookie)[0], code)
                        failed = len(self.dispatched(fake))
                        self.assertEqual(self.step(token, cookie)[0], 403, "consumed")
                        successor = self.fresh_token(cookie)
                        self.assertEqual(self.fresh_token(cookie), successor, "GET is stable")
                        self.assertEqual(len(self.dispatched(fake)), failed,
                                         "replay and recovery never dispatch")
                        fake.status, fake.mute_reply = CD_IDLE_STATUS, VOLUME_OK
                        self.elapse_interval()
                        self.assertEqual(self.step(successor, cookie)[0], 200)
                        self.assertEqual(len(self.dispatched(fake)), failed + 1)
                        self.elapse_interval()
                        self.assertEqual(self.step(successor, cookie)[0], 403)
                    finally:
                        fake.close()
                    self.assertEqual(len(self.dispatched(fake)), failed + 1)
                else:
                    effect = (xs8_gateway.MuteServiceError("TIMEOUT", "no reply")
                              if name == "TIMEOUT" else None)
                    with mock.patch.object(xs8_gateway, "send_volume_step",
                                           side_effect=effect) as dispatch, self.eligible():
                        if name == "VOLUME_BUSY lock":
                            with state["lock"]:
                                self.assertEqual(self.step(token, cookie)[0], code)
                        else:
                            if name == "TOO_FAST":
                                state["completed"] = time.monotonic()
                            self.assertEqual(self.step(token, cookie)[0], code)
                        failed = dispatch.call_count
                        self.assertEqual(self.step(token, cookie)[0], 403, "consumed")
                        successor = self.fresh_token(cookie)
                        self.assertNotEqual(successor, token)
                        self.assertEqual(dispatch.call_count, failed)
                        dispatch.side_effect = None
                        dispatch.return_value = {"volume_result": "TX_VERIFIED",
                                                 "receiver_execution": "UNVERIFIED"}
                        self.elapse_interval()
                        self.assertEqual(self.step(successor, cookie)[0], 200)
                        self.assertEqual(dispatch.call_count, failed + 1)
                        self.assertEqual(self.step(successor, cookie)[0], 403)
                    self.assertEqual(failed, 1 if name == "TIMEOUT" else 0)
                self.assertNotEqual(successor, token)

    def test_default_timeout_and_allowlist(self):
        with mock.patch.object(xs8_gateway, "_send_control",
                               return_value=VOLUME_OK.decode().strip()) as control:
            self.assertEqual(xs8_gateway.send_volume_step("/unused", "UP"), {
                "state": "IDLE", "stop_owed": "0", "context_may_be_open": "0",
                "receiver_execution": "UNVERIFIED", "volume_result": "TX_VERIFIED"})
        self.assertEqual(control.call_args.args[1], b"VOLUME_STEP UP\n")
        self.assertGreaterEqual(control.call_args.args[2], 3.0)
        self.assertFalse(control.call_args.kwargs["fake_only"])
        for direction in ("up", "LEFT", "UP\nMUTE", ""):
            with self.subTest(direction=direction):
                with mock.patch.object(xs8_gateway, "_send_control") as control:
                    with self.assertRaises(xs8_gateway.MuteServiceError):
                        xs8_gateway.send_volume_step("/unused", direction)
                control.assert_not_called()

    def test_cli_gate_requires_secure_socket(self):
        base = ["--systemd-socket", "--nginx-uid", "33",
                "--https-origin", "https://woodland-pi.local"]
        args = xs8_gateway.parse_args(["--enable-volume"] + base)
        self.assertTrue(args.enable_volume)
        self.assertFalse(xs8_gateway.parse_args([]).enable_volume)
        self.assertFalse(xs8_gateway.parse_args(["--enable-cd-track"] + base).enable_volume)
        for options in (("--enable-volume",),
                        ("--enable-volume", "--systemd-socket", "--nginx-uid", "33"),
                        ("--enable-volume", "--https-origin", "https://woodland-pi.local"),
                        tuple(["--enable-volume"] + base + ["--socket", self.socket_path]),
                        tuple(["--enable-volume", "--enable-offline-mute"] + base +
                              ["--socket", self.socket_path])):
            with self.subTest(options=options):
                with contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        xs8_gateway.parse_args(list(options))


class CdPlaybackGatewayTest(GatewayTest):
    def enable(self):
        handler = self.server.RequestHandlerClass
        handler.cd_playback_enabled = True
        handler.https_origin = "https://woodland-pi.local"
        self.server.proxy_peer_verified = True

    def post(self, path, token, cookie, body, **overrides):
        headers = {"Origin": "https://woodland-pi.local", "X-Forwarded-Proto": "https",
                   "X-Kenwood-Authenticated-User": "remote", "X-CSRF-Token": token,
                   "Cookie": cookie, "Content-Type": "application/json"}
        headers.update(overrides.pop("headers", {}))
        return self.request_full("POST", path, body=body, headers=headers)

    def playback(self, token, cookie, action="STOP", body=None):
        return self.post("/api/v1/cd/playback", token, cookie,
                         body if body is not None else json.dumps({"action": action}))

    def dispatched(self, fake):
        return [c for c in fake.commands if c.startswith(b"CD_")]

    def elapse_interval(self):
        state = getattr(self.server, "cd_track", None)
        if state:
            state["completed"] = float("-inf")

    def test_gate_off_and_capabilities(self):
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                mute_reply=CD_TRANSPORT_OK)
        self.assertEqual(self.playback("x", "x")[0], 501)
        self.assertEqual(self.request("POST", "/api/v1/cd/transport")[0], 501)
        operations = json.loads(self.request("GET", "/api/v1/capabilities")[1])["operations"]
        self.assertFalse(operations["cd_playback"]["enabled"])
        self.assertEqual(operations["cd_playback"]["available_actions"], ["PLAY_PAUSE", "STOP"])
        self.assertEqual(operations["cd_playback"]["provisional_actions"], ["PLAY_PAUSE"])
        self.assertIn("STOP", operations["cd_transport"]["blocked_actions"])
        self.enable()
        token, cookie = self.session()
        code, body, _ = self.request_full("GET", "/api/v1/capabilities", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        fake.close()
        operations = json.loads(body)["operations"]
        self.assertTrue(operations["cd_playback"]["enabled"])
        self.assertEqual(operations["cd_transport"]["blocked_actions"],
                         ["SEEK_FORWARD", "SEEK_BACKWARD"])
        self.assertFalse(operations["cd_transport"]["enabled"], "track gate stays separate")
        self.assertEqual(self.dispatched(fake), [])

    def test_actions_dispatch_one_exact_frame_command(self):
        self.enable()
        for action, command, provisional in (("PLAY_PAUSE", b"CD_PLAY_PAUSE\n", True),
                                             ("STOP", b"CD_STOP\n", False)):
            with self.subTest(action=action):
                token, cookie = self.session()
                self.elapse_interval()
                fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                        mute_reply=CD_TRANSPORT_OK.replace(b"STOP", action.encode()))
                code, body, _ = self.playback(token, cookie, action)
                replay = self.playback(token, cookie, action)[0]
                fake.close()
                self.assertEqual(code, 200)
                payload = json.loads(body)
                self.assertEqual((payload["result"], payload["action"], payload["gpio_tx"],
                                  payload["waveform"], payload["receiver_execution"]),
                                 ("SERVICE_TX_OK", action, "TX_OK", "MATCH", "UNVERIFIED"))
                self.assertIs(payload.get("mapping") == "PROVISIONAL", provisional)
                self.assertEqual(fake.commands, [b"STATUS\n", command])
                self.assertEqual(replay, 403)

    def test_invalid_actions_never_dispatch(self):
        self.enable()
        token, cookie = self.session()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS,
                                mute_reply=CD_TRANSPORT_OK)
        for body in ('{"action":"PLAY"}', '{"action":"NEXT"}', '{"action":"stop"}',
                     '{"action":"SEEK_FORWARD"}', '{"action":"STOP","x":1}', '{}', '[]'):
            with self.subTest(body=body):
                self.assertEqual(self.playback(token, cookie, body=body)[0], 400)
        fake.close()
        self.assertEqual(self.dispatched(fake), [])

    def test_status_gating_and_shared_cd_spacing(self):
        self.enable()
        self.server.RequestHandlerClass.cd_track_enabled = True
        for status, result in ((CD_HOLD_STATUS, "CD_BUSY"), (CD_FAULT_STATUS, "CD_FAULT_LATCHED"),
                               (STATUS, "NOT_ELIGIBLE"),
                               (CD_IDLE_STATUS.replace(b"OK state=IDLE", b"OK state=HOLDING_UP"),
                                "NOT_ELIGIBLE")):
            with self.subTest(result=result, status=status[-30:]):
                token, cookie = self.session()
                fake = RecordingService(self.socket_path, status=status, mute_reply=CD_TRANSPORT_OK)
                code, body, _ = self.playback(token, cookie)
                fake.close()
                self.assertEqual((code, json.loads(body)["result"]), (409, result))
                self.assertEqual(self.dispatched(fake), [])
        # PREV/NEXT and playback share one CD lock and one completion interval.
        self.server.RequestHandlerClass.cd_track_interval_ms = 1000
        token, cookie = self.session()
        self.elapse_interval()
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=CD_OK)
        try:
            self.assertEqual(self.post("/api/v1/cd/track", token, cookie,
                                       json.dumps({"action": "NEXT"}))[0], 200)
            code, body, _ = self.playback(self.fresh_token(cookie), cookie)
            self.assertEqual((code, json.loads(body)["result"]), (429, "TOO_FAST"))
            state = self.server.__dict__["cd_track"]
            self.elapse_interval()
            with state["lock"]:
                code, body, _ = self.playback(self.fresh_token(cookie), cookie)
            self.assertEqual((code, json.loads(body)["result"]), (409, "CD_BUSY"))
        finally:
            fake.close()
        self.assertEqual(self.dispatched(fake), [b"CD_TRACK_NEXT\n"])

    def fresh_token(self, cookie):
        code, body, _ = self.request_full("GET", "/api/v1/session", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        return json.loads(body)["csrf_token"]

    def test_uncertain_malformed_and_timeout_are_never_retried(self):
        self.enable()
        uncertain = (b"ERR CD_TRANSPORT_UNCERTAIN action=STOP gpio_tx=TX_OK waveform=MISSING "
                     b"receiver_execution=UNVERIFIED\n")
        for reply, code, result, check in (
                (uncertain, 502, "SERVICE_UNCERTAIN", True),
                (CD_TRANSPORT_OK.replace(b"action=STOP", b"action=PLAY_PAUSE"), 502,
                 "INVALID_REPLY", True),
                (CD_TRANSPORT_OK.replace(b"waveform=MATCH", b"waveform=MISSING"), 502,
                 "INVALID_REPLY", True),
                (CD_TRANSPORT_OK.replace(b" receiver_execution=UNVERIFIED", b""), 502,
                 "INVALID_REPLY", True),
                (b"ERR CD_FAULT_LATCHED\n", 409, "CD_FAULT_LATCHED", True),
                (b"ERR CD_TRACK_BUSY\n", 409, "REJECTED", False),
                (b"ERR UNKNOWN_COMMAND\n", 409, "REJECTED", False)):
            with self.subTest(reply=reply):
                token, cookie = self.session()
                self.elapse_interval()
                fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=reply)
                status, body, _ = self.playback(token, cookie)
                fake.close()
                payload = json.loads(body)
                self.assertEqual((status, payload["result"]), (code, result))
                self.assertIs(payload["physical_check_required"], check)
                self.assertNotIn("csrf_token", payload)
                self.assertEqual(self.dispatched(fake), [b"CD_STOP\n"])
        token, cookie = self.session()
        self.elapse_interval()
        with mock.patch.object(xs8_gateway, "read_status", return_value={
                "state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
                "context_may_be_open": "0", "cd_state": "IDLE", "cd_release_owed": "0"}):
            fake = FakeService(self.socket_path, CD_TRANSPORT_OK, delay=0.3)
            original = xs8_gateway.send_cd_playback
            with mock.patch.object(xs8_gateway, "send_cd_playback", side_effect=lambda path, a:
                                   original(path, a, timeout=0.1)) as dispatch:
                code, body, _ = self.playback(token, cookie)
            fake.close()
        self.assertEqual((code, json.loads(body)["result"]), (504, "TIMEOUT"))
        self.assertTrue(json.loads(body)["physical_check_required"])
        dispatch.assert_called_once()
        self.assertEqual(fake.commands, [b"CD_STOP\n"])

    def test_cli_gates(self):
        base = ["--systemd-socket", "--nginx-uid", "33", "--https-origin",
                "https://woodland-pi.local"]
        for flag, name in (("--enable-cd-playback", "enable_cd_playback"),
                           ("--enable-cd-seek", "enable_cd_seek")):
            with self.subTest(flag=flag):
                self.assertTrue(getattr(xs8_gateway.parse_args([flag] + base), name))
                self.assertFalse(getattr(xs8_gateway.parse_args([]), name))
                self.assertFalse(getattr(xs8_gateway.parse_args(["--enable-cd-track"] + base), name))
                for options in ([flag], [flag, "--systemd-socket", "--nginx-uid", "33"],
                                [flag] + base + ["--socket", self.socket_path],
                                [flag, "--enable-offline-mute"] + base +
                                ["--socket", self.socket_path]):
                    with contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaises(SystemExit):
                            xs8_gateway.parse_args(options)


class CdSeekGatewayTest(CdPlaybackGatewayTest):
    """Web CD seek on one persistent, connection-owned service socket."""

    def enable(self):
        handler = self.server.RequestHandlerClass
        handler.cd_seek_enabled = True
        handler.https_origin = "https://woodland-pi.local"
        self.server.proxy_peer_verified = True

    def setUp(self):
        super().setUp()
        self.fake = None

    def tearDown(self):
        super().tearDown()
        if self.fake is not None:
            self.fake.close()

    def service(self, **kwargs):
        self.fake = KeyService(self.socket_path, **kwargs)
        return self.fake

    def seek(self, token, cookie, body):
        code, raw, headers = self.post("/api/v1/cd/seek", token, cookie, json.dumps(body))
        return code, json.loads(raw), raw, headers

    def start(self, token, cookie, direction="FORWARD"):
        self.elapse_interval()
        return self.seek(token, cookie, {"action": "START", "direction": direction})

    def hold(self):
        return self.server.__dict__.get("cd_seek", {}).get("hold")

    def hold_connection(self):
        connections = self.fake.key_connections()
        self.assertEqual(len(connections), 1, "exactly one owning service connection")
        return connections[0]

    def session_object(self, cookie):
        return self.server.RequestHandlerClass.sessions[cookie.split("=", 1)[1]]

    # Inherited playback tests do not apply to this class.
    test_gate_off_and_capabilities = None
    test_actions_dispatch_one_exact_frame_command = None
    test_invalid_actions_never_dispatch = None
    test_status_gating_and_shared_cd_spacing = None
    test_uncertain_malformed_and_timeout_are_never_retried = None
    test_cli_gates = None

    def test_gate_off_and_capabilities_report_seek(self):
        fake = self.service()
        self.assertEqual(self.post("/api/v1/cd/seek", "x", "x", "{}")[0], 501)
        operations = json.loads(self.request("GET", "/api/v1/capabilities")[1])["operations"]
        self.assertFalse(operations["cd_seek"]["enabled"])
        self.enable()
        token, cookie = self.session()
        code, body, _ = self.request_full("GET", "/api/v1/capabilities", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        seek = json.loads(body)["operations"]["cd_seek"]
        self.assertEqual((seek["enabled"], seek["available_directions"], seek["heartbeat_ms"],
                          seek["lease_ms"], seek["max_hold_ms"]),
                         (True, ["FORWARD", "BACKWARD"], 500, 1200, 10000))
        self.assertEqual(fake.lines(), [])

    def test_persistent_connection_lifecycle(self):
        """START: one KEY_DOWN on a new socket that stays open. HEARTBEAT: the same
        KEY_DOWN on that socket (no new press). RELEASE: KEY_UP, then close."""
        self.enable()
        logged = []
        self.server.RequestHandlerClass.log_message = (
            lambda handler, fmt, *args: logged.append(fmt % args))
        for direction, action, press in (("FORWARD", "SEEK_FORWARD", "0x7A"),
                                         ("BACKWARD", "SEEK_BACKWARD", "0x6A")):
            with self.subTest(direction=direction):
                fake = self.service()
                token, cookie = self.session()
                code, payload, raw, headers = self.start(token, cookie, direction)
                self.assertEqual(code, 200)
                self.assertEqual((payload["result"], payload["direction"], payload["gpio_tx"],
                                  payload["waveform"], payload["receiver_execution"]),
                                 ("SEEK_STARTED", direction, "TX_OK", "MATCH", "UNVERIFIED"))
                self.assertNotIn(b"hold_token", raw)
                hold_id = payload["hold_id"]
                owner = self.hold_connection()
                # Kept referenced so only an explicit close (not garbage
                # collection) can end the service connection.
                connection = self.hold()["connection"]
                self.assertFalse(connection.closed)
                self.assertEqual(self.post("/api/v1/cd/seek", token, cookie, "{}")[0], 403)
                token = payload["csrf_token"]
                for _ in range(3):
                    code, payload, _, _ = self.seek(token, cookie,
                                                    {"action": "HEARTBEAT", "hold_id": hold_id})
                    self.assertEqual((code, payload["result"]), (200, "LEASE_RENEWED"))
                    token = payload["csrf_token"]
                self.assertEqual(fake.tx, [press], "heartbeats never press again")
                code, payload, _, _ = self.seek(token, cookie,
                                                {"action": "RELEASE", "hold_id": hold_id})
                self.assertEqual((code, payload["result"], payload["waveform"]),
                                 (200, "RELEASED", "MATCH"))
                fake.wait(lambda: (owner, "CLOSE") in fake.events)
                self.assertTrue(connection.closed)
                self.assertEqual(fake.lines(owner), [f"KEY_DOWN {action}"] * 4 +
                                 [f"KEY_UP {action}", "CLOSE"])
                self.assertEqual(fake.tx, [press, "0x72"])
                self.assertEqual(fake.key_connections(), [owner], "never reconnected")
                self.assertIsNone(self.hold())
                fake.close()
                self.fake = None
        log = "\n".join(logged)
        self.assertIn("CD_SEEK action=START code=200 result=SEEK_STARTED", log)
        self.assertNotIn(hold_id, log)

    def test_lost_start_response_release_needs_no_hold_id(self):
        self.enable()
        fake = self.service()
        token, cookie = self.session()
        self.assertEqual(self.start(token, cookie)[0], 200)  # Response discarded.
        owner = self.hold_connection()
        connection = self.hold()["connection"]  # Only an explicit close may end it.
        code, payload, _, _ = self.seek(self.fresh_token(cookie), cookie, {"action": "RELEASE"})
        self.assertEqual((code, payload["result"]), (200, "RELEASED"))
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertTrue(connection.closed)
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie, {"action": "RELEASE"})
        self.assertEqual((code, payload["result"]), (200, "NOT_ACTIVE"))
        self.assertEqual(fake.lines(owner), ["KEY_DOWN SEEK_FORWARD", "KEY_UP SEEK_FORWARD", "CLOSE"])
        self.assertEqual(fake.tx, ["0x7A", "0x72"])

    def test_undelivered_start_response_closes_the_connection(self):
        self.enable()
        fake = self.service()
        token, cookie = self.session()
        original = xs8_gateway.GatewayHandler.send_json

        def broken(handler, code, payload, headers=None):
            if payload.get("result") == "SEEK_STARTED":
                raise BrokenPipeError("browser went away")
            return original(handler, code, payload, headers)

        with mock.patch.object(xs8_gateway.GatewayHandler, "send_json", broken):
            self.elapse_interval()
            with self.assertRaises(Exception):
                self.seek(token, cookie, {"action": "START", "direction": "FORWARD"})
        owner = self.hold_connection()
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertEqual(fake.tx, ["0x7A", "0x72"], "released by closing the socket")
        self.assertEqual(fake.last, "CLIENT_DISCONNECT")
        self.assertIsNone(self.hold())

    def test_connection_loss_during_hold_is_never_reconnected(self):
        self.enable()
        fake = self.service()
        token, cookie = self.session()
        _, payload, _, _ = self.start(token, cookie)
        hold_id = payload["hold_id"]
        owner = self.hold_connection()
        fake.drop(owner)
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertEqual(fake.tx, ["0x7A", "0x72"])
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie,
                                        {"action": "HEARTBEAT", "hold_id": hold_id})
        self.assertEqual((code, payload["result"]), (502, "DISCONNECTED"))
        self.assertTrue(payload["physical_check_required"])
        self.assertIsNone(self.hold())
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie, {"action": "RELEASE"})
        self.assertEqual((code, payload["result"]), (200, "NOT_ACTIVE"))
        self.assertEqual(fake.key_connections(), [owner], "no new connection")
        self.assertEqual(sum(line.startswith("KEY_DOWN") for line in fake.lines()), 1)
        self.assertEqual(fake.tx, ["0x7A", "0x72"])

    def test_browser_inactivity_closes_the_connection(self):
        self.enable()
        fake = self.service()
        token, cookie = self.session()
        _, payload, _, _ = self.start(token, cookie)
        owner = self.hold_connection()
        started = time.monotonic()
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertGreaterEqual(time.monotonic() - started, 1.2)
        self.assertEqual(fake.tx, ["0x7A", "0x72"])
        self.assertIsNone(self.hold())
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie,
                                        {"action": "HEARTBEAT", "hold_id": payload["hold_id"]})
        self.assertEqual((code, payload["result"]), (409, "NOT_ACTIVE"))
        self.assertEqual(fake.lines(owner), ["KEY_DOWN SEEK_FORWARD", "CLOSE"])

    def test_session_expiry_or_invalidation_closes_the_connection(self):
        self.enable()
        for invalidate in ("expire", "remove"):
            with self.subTest(invalidate=invalidate):
                fake = self.service()
                token, cookie = self.session()
                self.start(token, cookie)
                owner = self.hold_connection()
                if invalidate == "expire":
                    self.session_object(cookie)["expires"] = 0
                else:
                    del self.server.RequestHandlerClass.sessions[cookie.split("=", 1)[1]]
                fake.wait(lambda: (owner, "CLOSE") in fake.events, timeout=1.4)
                self.assertEqual(fake.tx, ["0x7A", "0x72"])
                self.assertIsNone(self.hold())
                fake.close()
                self.fake = None

    def test_absolute_max_hold_closes_the_connection(self):
        self.enable()
        fake = self.service()
        token, cookie = self.session()
        _, payload, _, _ = self.start(token, cookie)
        owner = self.hold_connection()
        # A heartbeat at the maximum is not forwarded; the connection closes.
        self.hold()["started"] -= 10.0
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie,
                                        {"action": "HEARTBEAT", "hold_id": payload["hold_id"]})
        # The heartbeat or the service_actions() cleanup reaches the maximum first.
        self.assertEqual((code, payload["result"]), (409, "NOT_ACTIVE"))
        self.assertIn(payload.get("reason"), ("MAX_HOLD", None))
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertEqual(fake.lines(owner), ["KEY_DOWN SEEK_FORWARD", "CLOSE"])
        # The cleanup loop enforces the maximum even while heartbeats continue.
        self.fake.close()
        fake = self.service()
        _, payload, _, _ = self.start(payload["csrf_token"], cookie)
        second = self.hold_connection()
        self.hold()["started"] -= 10.0
        self.hold()["browser_seen"] = time.monotonic() + 60
        fake.wait(lambda: (second, "CLOSE") in fake.events, timeout=1.4)
        self.assertEqual(fake.tx, ["0x7A", "0x72"])

    def test_late_heartbeat_after_service_expiry_never_restarts(self):
        self.enable()
        fake = self.service()
        token, cookie = self.session()
        _, payload, _, _ = self.start(token, cookie)
        owner = self.hold_connection()
        fake.expire("INACTIVITY")
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie,
                                        {"action": "HEARTBEAT", "hold_id": payload["hold_id"]})
        self.assertEqual((code, payload["result"], payload["cd_last_result"]),
                         (409, "NOT_ACTIVE", "INACTIVITY"))
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertEqual(fake.tx, ["0x7A", "0x72"], "no second press")
        self.assertEqual(fake.lines(owner), ["KEY_DOWN SEEK_FORWARD", "KEY_DOWN SEEK_FORWARD",
                                             "CLOSE"])
        self.assertIsNone(self.hold())

    def test_stale_hold_ids_and_other_sessions_never_touch_the_socket(self):
        self.enable()
        fake = self.service()
        token, cookie = self.session()
        _, payload, _, _ = self.start(token, cookie)
        hold_id, mine = payload["hold_id"], payload["csrf_token"]
        owner = self.hold_connection()
        other_token, other_cookie = self.session()
        for body in ({"action": "HEARTBEAT", "hold_id": hold_id},
                     {"action": "RELEASE", "hold_id": hold_id}, {"action": "RELEASE"}):
            code, payload, _, _ = self.seek(other_token, other_cookie, body)
            other_token = payload["csrf_token"]
            self.assertIn((code, payload["result"]), ((409, "NOT_ACTIVE"), (200, "NOT_ACTIVE")))
        code, payload, _, _ = self.seek(mine, cookie, {"action": "HEARTBEAT",
                                                       "hold_id": "stale-hold"})
        self.assertEqual((code, payload["result"]), (409, "NOT_ACTIVE"))
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie,
                                        {"action": "RELEASE", "hold_id": "stale-hold"})
        self.assertEqual((code, payload["result"]), (409, "NOT_OWNER"))
        self.assertEqual(fake.lines(owner), ["KEY_DOWN SEEK_FORWARD"])
        self.assertIsNotNone(self.hold())
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie,
                                        {"action": "RELEASE", "hold_id": hold_id})
        self.assertEqual(payload["result"], "RELEASED")
        # The finished hold's id cannot release a newer hold.
        _, payload, _, _ = self.start(payload["csrf_token"], cookie)
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie,
                                        {"action": "RELEASE", "hold_id": hold_id})
        self.assertEqual((code, payload["result"]), (409, "NOT_OWNER"))
        self.assertIsNotNone(self.hold())
        self.assertEqual(fake.tx, ["0x7A", "0x72", "0x7A"])

    def test_gateway_shutdown_closes_the_connection(self):
        self.enable()
        fake = self.service()
        token, cookie = self.session()
        self.start(token, cookie)
        owner = self.hold_connection()
        self.server.shutdown()
        self.server.server_close()
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertEqual(fake.tx, ["0x7A", "0x72"])
        self.assertEqual(fake.last, "CLIENT_DISCONNECT")

    def test_mutual_exclusion_with_other_cd_actions(self):
        self.enable()
        handler = self.server.RequestHandlerClass
        handler.cd_track_enabled = True
        handler.cd_playback_enabled = True
        fake = self.service()
        token, cookie = self.session()
        _, payload, _, _ = self.start(token, cookie)
        token = payload["csrf_token"]
        for path, body in (("/api/v1/cd/track", {"action": "NEXT"}),
                           ("/api/v1/cd/playback", {"action": "STOP"})):
            self.elapse_interval()
            code, raw, _ = self.post(path, token, cookie, json.dumps(body))
            self.assertEqual((code, json.loads(raw)["result"]), (409, "CD_BUSY"))
            token = self.fresh_token(cookie)
        code, payload, _, _ = self.start(token, cookie, "BACKWARD")
        self.assertEqual((code, payload["result"]), (409, "CD_BUSY"))
        self.assertEqual(fake.lines(), ["KEY_DOWN SEEK_FORWARD"])
        # If the service ended the hold on its own, the dead connection is
        # closed and a new START is allowed.
        fake.expire("INACTIVITY")
        first = self.hold_connection()
        code, payload, _, _ = self.start(payload["csrf_token"], cookie, "BACKWARD")
        self.assertEqual(code, 200)
        fake.wait(lambda: (first, "CLOSE") in fake.events)
        self.assertEqual(fake.tx, ["0x7A", "0x72", "0x6A"])

    def test_start_failures_close_the_connection_and_never_retry(self):
        self.enable()
        uncertain = (b"ERR KEY_DOWN_UNCERTAIN action=SEEK_FORWARD gpio_tx=TX_OK "
                     b"waveform=MISSING release_gpio_tx=TX_OK release_waveform=MATCH "
                     b"receiver_execution=UNVERIFIED\n")
        ok = (b"OK KEY_DOWN_TX action=SEEK_FORWARD gpio_tx=TX_OK waveform=MATCH "
              b"receiver_execution=UNVERIFIED\n")
        for reply, code, result, check in (
                (uncertain, 502, "SERVICE_UNCERTAIN", True),
                (ok.replace(b"waveform=MATCH", b"waveform=MISSING"), 502, "INVALID_REPLY", True),
                (ok.replace(b"SEEK_FORWARD", b"SEEK_BACKWARD"), 502, "INVALID_REPLY", True),
                (ok.replace(b"\n", b"\nOK EXTRA\n"), 502, "INVALID_REPLY", True),
                (b"ERR KEY_BUSY\n", 409, "REJECTED", False),
                (b"ERR KEY_EXPIRED last=INACTIVITY\n", 409, "REJECTED", False),
                (b"ERR CD_FAULT_LATCHED\n", 409, "CD_FAULT_LATCHED", True),
                (None, 502, "DISCONNECTED", True)):
            with self.subTest(reply=reply):
                fake = self.service()
                fake.script["KEY_DOWN"] = [reply]
                token, cookie = self.session()
                status, payload, _, _ = self.start(token, cookie)
                self.assertEqual((status, payload["result"]), (code, result))
                self.assertIs(payload["physical_check_required"], check)
                self.assertEqual(payload["receiver_execution"], "UNVERIFIED")
                self.assertIn("csrf_token", payload)
                self.assertIsNone(self.hold())
                owner = self.hold_connection()
                fake.wait(lambda: (owner, "CLOSE") in fake.events)
                self.assertEqual(fake.lines(owner), ["KEY_DOWN SEEK_FORWARD", "CLOSE"])
                fake.close()
                self.fake = None
        # Reply timeout: closed (the service releases any press), not retried.
        fake = self.service()
        ok_line = ok
        fake.script["KEY_DOWN"] = [("delay", 0.5, ok_line)]
        original = xs8_gateway.ServiceConnection.exchange
        token, cookie = self.session()
        with mock.patch.object(xs8_gateway.ServiceConnection, "exchange",
                               lambda conn, line, timeout, max_reply=512:
                               original(conn, line, 0.1, max_reply)):
            status, payload, _, _ = self.start(token, cookie)
        self.assertEqual((status, payload["result"]), (504, "TIMEOUT"))
        self.assertTrue(payload["physical_check_required"])
        owner = self.hold_connection()
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertEqual(fake.lines(owner), ["KEY_DOWN SEEK_FORWARD", "CLOSE"])
        fake.close()
        self.fake = None
        # No service at all.
        token, cookie = self.session()
        self.elapse_interval()
        with mock.patch.object(xs8_gateway, "read_status", return_value={
                "state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
                "context_may_be_open": "0", "cd_state": "IDLE", "cd_release_owed": "0"}):
            status, payload, _, _ = self.start(token, cookie)
        self.assertEqual((status, payload["result"]), (503, "UNAVAILABLE"))
        self.assertFalse(payload["physical_check_required"])

    def test_release_and_heartbeat_faults_close_the_connection(self):
        self.enable()
        uncertain = (b"ERR KEY_UP_UNCERTAIN action=SEEK_FORWARD gpio_tx=TX_OK "
                     b"waveform=MISSING receiver_execution=UNVERIFIED\n")
        for verb, reply, code, result in (
                ("KEY_UP", uncertain, 502, "SERVICE_UNCERTAIN"),
                ("KEY_UP", b"OK KEY_UP_TX action=SEEK_FORWARD\n", 502, "INVALID_REPLY"),
                ("KEY_UP", None, 502, "DISCONNECTED"),
                ("HEARTBEAT", b"OK KEY_ALIVE action=SEEK_BACKWARD\n", 502, "INVALID_REPLY"),
                ("HEARTBEAT", b"ERR KEY_BUSY\n", 502, "INVALID_REPLY"),
                ("HEARTBEAT", None, 502, "DISCONNECTED")):
            with self.subTest(verb=verb, reply=reply):
                fake = self.service()
                token, cookie = self.session()
                _, payload, _, _ = self.start(token, cookie)
                owner = self.hold_connection()
                fake.script["KEY_UP" if verb == "KEY_UP" else "KEY_DOWN"] = [reply]
                body = ({"action": "RELEASE", "hold_id": payload["hold_id"]} if verb == "KEY_UP"
                        else {"action": "HEARTBEAT", "hold_id": payload["hold_id"]})
                status, payload, _, _ = self.seek(payload["csrf_token"], cookie, body)
                self.assertEqual((status, payload["result"]), (code, result))
                self.assertTrue(payload["physical_check_required"])
                self.assertIsNone(self.hold())
                fake.wait(lambda: (owner, "CLOSE") in fake.events)
                # Closing released whatever the scripted reply left held; nothing resent.
                self.assertEqual(sum(l.startswith("KEY_UP") for l in fake.lines()),
                                 1 if verb == "KEY_UP" else 0)
                self.assertEqual(fake.key_connections(), [owner])
                self.assertIsNone(fake.hold)
                fake.close()
                self.fake = None

    def test_invalid_bodies_never_contact_the_service(self):
        self.enable()
        fake = self.service()
        token, cookie = self.session()
        for body in ('{"action":"START"}', '{"action":"START","direction":"UP"}',
                     '{"action":"START","direction":"FORWARD","x":"1"}',
                     '{"action":"HEARTBEAT"}', '{"action":"HEARTBEAT","hold_id":""}',
                     '{"action":"HEARTBEAT","hold_id":"a b"}', '{"action":"HEARTBEAT","hold_id":1}',
                     '{"action":"RELEASE","action":"RELEASE"}', '{"action":"STOP"}', '{}', '[]',
                     '"RELEASE"', 'x'):
            with self.subTest(body=body):
                self.assertEqual(self.post("/api/v1/cd/seek", token, cookie, body)[0], 400)
        # A 400 is rejected before the token is consumed.
        self.assertEqual(self.seek(token, cookie, {"action": "RELEASE"})[0], 200)
        self.assertEqual(fake.lines(), [])


class RealServiceSeekTest(CdSeekGatewayTest):
    """The Gateway against the real XS8 Service socket loop (fake GPIO backend)."""

    BINARY = Path(__file__).resolve().parent.parent / ".build" / "xs8_service_socket_test"

    def setUp(self):
        if not self.BINARY.exists():
            self.skipTest("run make test first to build the service socket test binary")
        super().setUp()
        self.log_path = Path(self.temp.name) / "service.log"
        self.log = self.log_path.open("w")
        self.process = subprocess.Popen(
            [str(self.BINARY), "--socket", self.socket_path, "--allow-unrecovered-start"],
            stdout=subprocess.DEVNULL, stderr=self.log)
        deadline = time.monotonic() + 3
        while not Path(self.socket_path).exists():
            self.assertLess(time.monotonic(), deadline, "service did not start")
            time.sleep(0.01)

    def tearDown(self):
        super().tearDown()
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=3)
        self.log.close()

    def transmitted(self):
        return re.findall(r"^FAKE_TX (0x[0-9A-F]{2})$", self.log_path.read_text(), re.M)

    def wait_tx(self, expected, timeout=3):
        deadline = time.monotonic() + timeout
        while self.transmitted() != expected:
            self.assertLess(time.monotonic(), deadline, self.transmitted())
            time.sleep(0.02)

    # Fake-specific tests do not apply.
    for _name in [n for n in dir(CdSeekGatewayTest) if n.startswith("test_")]:
        locals()[_name] = None
    del _name

    def test_real_service_start_keepalive_release(self):
        self.enable()
        token, cookie = self.session()
        code, payload, _, _ = self.start(token, cookie, "BACKWARD")
        self.assertEqual((code, payload["result"]), (200, "SEEK_STARTED"), payload)
        hold_id = payload["hold_id"]
        for _ in range(3):
            time.sleep(0.4)
            code, payload, _, _ = self.seek(payload["csrf_token"], cookie,
                                            {"action": "HEARTBEAT", "hold_id": hold_id})
            self.assertEqual((code, payload["result"]), (200, "LEASE_RENEWED"), payload)
        self.assertEqual(self.transmitted(), ["0x6A"])
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie,
                                        {"action": "RELEASE", "hold_id": hold_id})
        self.assertEqual((code, payload["result"], payload["waveform"]),
                         (200, "RELEASED", "MATCH"), payload)
        self.assertEqual(self.transmitted(), ["0x6A", "0x72"])
        self.assertIn("cd_last_result=CLIENT_RELEASE", self.service_status())

    def service_status(self):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(1)
            client.connect(self.socket_path)
            client.sendall(b"STATUS\n")
            reply = b""
            while not reply.endswith(b"\n"):
                reply += client.recv(1024)
            return reply.decode()

    def test_real_service_releases_on_gateway_shutdown_and_expiry(self):
        self.enable()
        token, cookie = self.session()
        code, payload, _, _ = self.start(token, cookie)
        self.assertEqual(code, 200, payload)
        self.server.shutdown()
        self.server.server_close()
        self.wait_tx(["0x7A", "0x72"])
        self.assertIn("cd_last_result=CLIENT_DISCONNECT", self.service_status())

    def test_real_service_inactivity_then_late_heartbeat(self):
        self.enable()
        token, cookie = self.session()
        _, payload, _, _ = self.start(token, cookie)
        # Hold the Gateway's own cleanup off so the service's 1200 ms
        # inactivity limit acts first, then send a late heartbeat.
        self.hold()["browser_seen"] = time.monotonic() + 60
        self.wait_tx(["0x7A", "0x72"])
        self.assertIn("cd_last_result=INACTIVITY", self.service_status())
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie,
                                        {"action": "HEARTBEAT", "hold_id": payload["hold_id"]})
        self.assertEqual((code, payload["result"], payload["cd_last_result"]),
                         (409, "NOT_ACTIVE", "INACTIVITY"))
        time.sleep(0.3)
        self.assertEqual(self.transmitted(), ["0x7A", "0x72"], "never a second press")
        self.assertIsNone(self.hold())


SOURCE_OK = b"OK SOURCE_TX gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED\n"
POWER_OK = (b"OK POWER_TX action=ON gpio_tx=TX_OK waveform=MATCH "
            b"receiver_execution=UNVERIFIED\n")
MONITOR_OK = (b"OK TAPE2_MONITOR_TX gpio_tx=TX_OK waveform=MATCH "
              b"receiver_execution=UNVERIFIED\n")
VOLUME_HOLDING = CD_IDLE_STATUS.replace(b"OK state=IDLE", b"OK state=HOLDING_UP")


class RemoteV2GatewayTest(CdSeekGatewayTest):
    """VIDEO1/VIDEO2 sources, POWER, TAPE2 MONITOR and held Web volume."""

    for _name in [n for n in dir(CdSeekGatewayTest) if n.startswith("test_")]:
        locals()[_name] = None
    del _name

    def gate(self, **flags):
        handler = self.server.RequestHandlerClass
        for name, value in flags.items():
            setattr(handler, name, value)
        handler.https_origin = "https://woodland-pi.local"
        self.server.proxy_peer_verified = True

    def dispatched(self, fake):
        return [c for c in fake.commands if c != b"STATUS\n"]

    def spaced(self, name):
        state = self.server.__dict__.get(name)
        if state:
            state["completed"] = float("-inf")

    def operations(self, cookie):
        _, body, _ = self.request_full("GET", "/api/v1/capabilities", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        return json.loads(body)["operations"]

    def session_flags(self, cookie):
        _, body, _ = self.request_full("GET", "/api/v1/session", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        return json.loads(body)

    # ---- VIDEO1 / VIDEO2 ----
    def test_tv_sources_are_single_atomic_service_commands(self):
        self.gate(production_source_enabled=True)
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=SOURCE_OK)
        try:
            token, cookie = self.session()
            source = self.operations(cookie)["source"]
            self.assertEqual(source["available_sources"],
                             ["CD", "TUNER", "PHONO", "VIDEO1", "VIDEO2", "TAPE1"])
            self.assertEqual(source["blocked_sources"], ["TAPE2"])
            for name in ("VIDEO1", "VIDEO2"):
                code, body, _ = self.source(token, cookie, name)
                self.assertEqual(code, 200, body)
                self.assertEqual(json.loads(body)["receiver_execution"], "UNVERIFIED")
                time.sleep(1.0)  # The existing per-session source spacing.
                token = self.fresh_token(cookie)
            code, body, _ = self.source(token, cookie, "TAPE2")
            self.assertEqual((code, json.loads(body)["result"]), (409, "SOURCE_UNVERIFIED"))
        finally:
            fake.close()
        # One service command each: the CONTEXT sequence is the service's job.
        self.assertEqual(self.dispatched(fake), [b"SELECT_SOURCE VIDEO1\n", b"SELECT_SOURCE VIDEO2\n"])
        with mock.patch.object(xs8_gateway, "_send_control",
                               return_value=SOURCE_OK.decode().strip()) as control:
            xs8_gateway.send_source("/unused", "VIDEO2")
        self.assertGreaterEqual(control.call_args.args[2], 3.0, "three frames need time")

    def test_tv_source_uncertainty_is_reported_not_retried(self):
        self.gate(production_source_enabled=True)
        reply = (b"ERR SOURCE_UNCERTAIN gpio_tx=TX_OK waveform=MISSING "
                 b"receiver_execution=UNVERIFIED\n")
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=reply)
        token, cookie = self.session()
        code, body, _ = self.source(token, cookie, "VIDEO1")
        fake.close()
        self.assertEqual((code, json.loads(body)["result"]), (502, "SERVICE_UNCERTAIN"))
        self.assertEqual(self.dispatched(fake), [b"SELECT_SOURCE VIDEO1\n"])

    # ---- POWER ----
    def power(self, token, cookie, action, body=None):
        return self.post("/api/v1/power", token, cookie,
                         body if body is not None else json.dumps({"action": action}))

    def test_power_gate_capability_and_exact_commands(self):
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=POWER_OK)
        try:
            self.assertEqual(self.power("x", "x", "ON")[0], 501)
            self.assertFalse(json.loads(self.request("GET", "/api/v1/capabilities")[1])
                             ["operations"]["power"]["enabled"])
            self.gate(power_enabled=True)
            token, cookie = self.session()
            self.assertTrue(self.session_flags(cookie)["power_available"])
            power = self.operations(cookie)["power"]
            self.assertEqual((power["enabled"], power["available_actions"],
                              power["provisional_actions"]), (True, ["ON", "OFF"], ["OFF"]))
            for action in ("ON", "OFF"):
                fake.mute_reply = POWER_OK.replace(b"action=ON", f"action={action}".encode())
                self.spaced("power")
                code, body, _ = self.power(token, cookie, action)
                payload = json.loads(body)
                self.assertEqual((code, payload["result"], payload["action"],
                                  payload["receiver_execution"]),
                                 (200, "SERVICE_TX_OK", action, "UNVERIFIED"))
                self.assertIs(payload.get("mapping") == "PROVISIONAL", action == "OFF")
                self.assertEqual(self.power(token, cookie, action)[0], 403, "single use")
                token = self.fresh_token(cookie)
            for body in ('{"action":"on"}', '{"action":"TOGGLE"}', '{"action":"STANDBY"}',
                         '{"action":"0x39"}', '{}', '{"action":"ON","x":1}'):
                self.assertEqual(self.power(token, cookie, None, body)[0], 400, body)
            # Spacing: a second POWER within 2 s is refused before dispatch.
            code, body, _ = self.power(token, cookie, "ON")
            self.assertEqual((code, json.loads(body)["result"]), (429, "TOO_FAST"))
        finally:
            fake.close()
        self.assertEqual(self.dispatched(fake), [b"POWER_ON\n", b"POWER_OFF\n"])

    def test_power_refused_during_holds_and_faults(self):
        self.gate(power_enabled=True)
        cases = {
            "volume_hold": (VOLUME_HOLDING, "VOLUME_BUSY"),
            "cd_hold": (CD_HOLD_STATUS, "CD_BUSY"),
            "cd_fault": (CD_FAULT_STATUS, "CD_FAULT_LATCHED"),
            "volume_fault": (CD_IDLE_STATUS.replace(b"OK state=IDLE", b"OK state=FAULT_LATCHED"),
                             "FAULT_LATCHED"),
            "stop_owed": (CD_IDLE_STATUS.replace(b"stop_owed=0", b"stop_owed=1"), "NOT_ELIGIBLE"),
            "context": (CD_IDLE_STATUS.replace(b"context_may_be_open=0",
                                               b"context_may_be_open=1"), "NOT_ELIGIBLE"),
            "boot": (CD_IDLE_STATUS.replace(b"OK state=IDLE", b"OK state=BOOT_INHIBIT"),
                     "NOT_ELIGIBLE"),
            "no_cd_fields": (STATUS, "NOT_ELIGIBLE"),
        }
        for name, (status, result) in cases.items():
            with self.subTest(case=name):
                token, cookie = self.session()
                self.spaced("power")
                fake = RecordingService(self.socket_path, status=status, mute_reply=POWER_OK)
                code, body, _ = self.power(token, cookie, "OFF")
                fake.close()
                self.assertEqual((code, json.loads(body)["result"]), (409, result))
                self.assertEqual(self.dispatched(fake), [])

    def test_power_uncertain_and_timeout_never_retried(self):
        self.gate(power_enabled=True)
        uncertain = (b"ERR POWER_UNCERTAIN action=OFF gpio_tx=TX_OK waveform=MISSING "
                     b"receiver_execution=UNVERIFIED\n")
        for reply, code, result, check in (
                (uncertain, 502, "SERVICE_UNCERTAIN", True),
                (POWER_OK, 502, "INVALID_REPLY", True),  # action=ON for an OFF request
                (POWER_OK.replace(b"waveform=MATCH", b"waveform=MISSING"), 502,
                 "INVALID_REPLY", True),
                (b"ERR HOLDING_UP\n", 409, "REJECTED", False),
                (b"ERR CD_FAULT_LATCHED\n", 409, "CD_FAULT_LATCHED", True)):
            with self.subTest(reply=reply):
                token, cookie = self.session()
                self.spaced("power")
                fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=reply)
                status, body, _ = self.power(token, cookie, "OFF")
                fake.close()
                payload = json.loads(body)
                self.assertEqual((status, payload["result"]), (code, result))
                self.assertIs(payload["physical_check_required"], check)
                self.assertEqual(self.dispatched(fake), [b"POWER_OFF\n"])

    # ---- TAPE2 MONITOR ----
    def monitor(self, token, cookie, body="{}"):
        return self.post("/api/v1/receiver/monitor", token, cookie, body)

    def test_tape2_monitor_is_gated_provisional_and_dispatches_once(self):
        fake = RecordingService(self.socket_path, status=CD_IDLE_STATUS, mute_reply=MONITOR_OK)
        try:
            self.assertEqual(self.monitor("x", "x")[0], 501)
            self.gate(tape2_monitor_enabled=True)
            token, cookie = self.session()
            self.assertTrue(self.session_flags(cookie)["tape2_monitor_available"])
            monitor = self.operations(cookie)["tape2_monitor"]
            self.assertTrue(monitor["enabled"] and monitor["provisional"])
            self.assertEqual(self.monitor(token, cookie, '{"on":true}')[0], 400)
            code, body, _ = self.monitor(token, cookie)
            payload = json.loads(body)
            self.assertEqual((code, payload["result"], payload["mapping"],
                              payload["receiver_execution"]),
                             (200, "SERVICE_TX_OK", "PROVISIONAL", "UNVERIFIED"))
            self.assertNotIn("monitor", {k.lower() for k in payload} - {"mapping"},
                             "a TX never reports a MONITOR state")
            code, body, _ = self.monitor(self.fresh_token(cookie), cookie)
            self.assertEqual((code, json.loads(body)["result"]), (429, "TOO_FAST"))
            fake.status = VOLUME_HOLDING
            self.spaced("tape2_monitor")
            code, body, _ = self.monitor(self.fresh_token(cookie), cookie)
            self.assertEqual((code, json.loads(body)["result"]), (409, "VOLUME_BUSY"))
            fake.status = CD_IDLE_STATUS
            fake.mute_reply = (b"ERR TAPE2_MONITOR_UNCERTAIN gpio_tx=TX_OK waveform=MISSING "
                               b"receiver_execution=UNVERIFIED\n")
            self.spaced("tape2_monitor")
            code, body, _ = self.monitor(self.fresh_token(cookie), cookie)
            self.assertEqual((code, json.loads(body)["result"]), (502, "SERVICE_UNCERTAIN"))
            self.assertTrue(json.loads(body)["physical_check_required"])
        finally:
            fake.close()
        self.assertEqual(self.dispatched(fake), [b"TAPE2_MONITOR_TOGGLE\n"] * 2)

    # ---- Web volume hold ----
    def hold_request(self, token, cookie, body):
        code, raw, headers = self.post("/api/v1/volume/hold", token, cookie, json.dumps(body))
        return code, json.loads(raw), raw, headers

    def start_volume(self, token, cookie, direction="UP"):
        self.spaced("volume_step")
        return self.hold_request(token, cookie, {"action": "START", "direction": direction})

    def test_volume_hold_lifecycle_on_one_connection(self):
        self.assertEqual(self.post("/api/v1/volume/hold", "x", "x", "{}")[0], 501)
        self.gate(volume_hold_enabled=True)
        for direction, frames in (("UP", ["0x17", "0xF9"]), ("DOWN", ["0x17", "0xE9"])):
            with self.subTest(direction=direction):
                fake = self.service()
                token, cookie = self.session()
                hold = self.operations(cookie)["volume_hold"]
                self.assertEqual((hold["enabled"], hold["available_directions"],
                                  hold["hold_delay_ms"], hold["max_hold_ms"]),
                                 (True, ["UP", "DOWN"], 400, 20000))
                self.assertEqual((hold["heartbeat_ms"], hold["lease_ms"]), (500, 1200))
                code, payload, _, _ = self.start_volume(token, cookie, direction)
                self.assertEqual((code, payload["result"]), (200, "HOLD_STARTED"), payload)
                owner = self.hold_connection()
                connection = self.hold()["connection"]
                for _ in range(3):
                    code, payload, _, _ = self.hold_request(
                        payload["csrf_token"], cookie,
                        {"action": "HEARTBEAT", "hold_id": payload["hold_id"]
                         if "hold_id" in payload else self.hold()["hold_id"]})
                    self.assertEqual((code, payload["result"]), (200, "LEASE_RENEWED"))
                self.assertEqual(fake.tx, frames, "keepalives never press again")
                code, payload, _, _ = self.hold_request(payload["csrf_token"], cookie,
                                                        {"action": "RELEASE"})
                self.assertEqual((code, payload["result"]), (200, "RELEASED"))
                fake.wait(lambda: (owner, "CLOSE") in fake.events)
                self.assertTrue(connection.closed)
                action = "VOLUME_" + direction
                self.assertEqual(fake.lines(owner), [f"KEY_DOWN {action}"] * 4 +
                                 [f"KEY_UP {action}", "CLOSE"])
                self.assertEqual(fake.tx, frames + ["0x09", "0x07"])
                fake.close()
                self.fake = None

    def test_volume_hold_closing_and_expiry_paths(self):
        self.gate(volume_hold_enabled=True)
        # Browser silence: the cleanup closes the socket; the service stops at once.
        fake = self.service()
        token, cookie = self.session()
        _, payload, _, _ = self.start_volume(token, cookie)
        owner = self.hold_connection()
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertEqual(fake.tx, ["0x17", "0xF9", "0x09", "0x07"])
        fake.close()
        # Gateway maximum (20 s): renewal refused, socket closed.
        fake = self.service()
        _, payload, _, _ = self.start_volume(payload["csrf_token"], cookie)
        owner = self.hold_connection()
        self.hold()["started"] -= 20.0
        code, payload, _, _ = self.hold_request(payload["csrf_token"], cookie, {
            "action": "HEARTBEAT", "hold_id": payload["hold_id"]})
        # Either the heartbeat sees the maximum (reason MAX_HOLD) or the
        # service_actions() cleanup closed it first; both refuse without forwarding.
        self.assertEqual((code, payload["result"]), (409, "NOT_ACTIVE"))
        self.assertIn(payload.get("reason"), ("MAX_HOLD", None))
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertEqual(fake.lines(owner), ["KEY_DOWN VOLUME_UP", "CLOSE"])
        fake.close()
        # Service watchdog then a late keepalive: KEY_EXPIRED, never a new press.
        fake = self.service()
        _, payload, _, _ = self.start_volume(payload["csrf_token"], cookie, "DOWN")
        owner = self.hold_connection()
        fake.expire("VOLUME_WATCHDOG")
        code, payload, _, _ = self.hold_request(payload["csrf_token"], cookie, {
            "action": "HEARTBEAT", "hold_id": payload["hold_id"]})
        self.assertEqual((code, payload["result"], payload["cd_last_result"]),
                         (409, "NOT_ACTIVE", "VOLUME_WATCHDOG"))
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertEqual(fake.tx, ["0x17", "0xE9", "0x09", "0x07"])

    def test_volume_hold_maximum_boundary_is_20_seconds(self):
        # A frozen Gateway clock makes the boundary exact; 1000.0 - 20.0 is exact.
        clock = {"now": 1000.0}
        frozen = mock.Mock(monotonic=lambda: clock["now"], monotonic_ns=time.monotonic_ns)
        self.gate(volume_hold_enabled=True)
        fake = self.service()
        token, cookie = self.session()
        _, payload, _, _ = self.start_volume(token, cookie)
        owner = self.hold_connection()
        with mock.patch.object(xs8_gateway, "time", frozen):
            hold = self.hold()
            hold["started"], hold["browser_seen"] = 1000.0 - 19.999, 1000.0
            # 19,999 ms: the cleanup keeps the hold and a heartbeat still renews it.
            self.server.service_actions()
            self.assertIs(self.hold(), hold)
            code, payload, _, _ = self.hold_request(payload["csrf_token"], cookie, {
                "action": "HEARTBEAT", "hold_id": hold["hold_id"]})
            self.assertEqual((code, payload["result"]), (200, "LEASE_RENEWED"))
            # 20,000 ms: the cleanup closes the owning connection (the service
            # stops at once) and the next heartbeat is refused without forwarding.
            hold["started"] = 1000.0 - 20.0
            self.server.service_actions()
            self.assertIsNone(self.hold())
            code, payload, _, _ = self.hold_request(payload["csrf_token"], cookie, {
                "action": "HEARTBEAT", "hold_id": hold["hold_id"]})
            self.assertEqual((code, payload["result"]), (409, "NOT_ACTIVE"))
        fake.wait(lambda: (owner, "CLOSE") in fake.events)
        self.assertEqual(fake.lines(owner), ["KEY_DOWN VOLUME_UP"] * 2 + ["CLOSE"])
        self.assertEqual(fake.tx, ["0x17", "0xF9", "0x09", "0x07"], "never a new press")
        # The heartbeat path is an independent backstop at the same boundary.
        _, payload, _, _ = self.start_volume(payload["csrf_token"], cookie, "DOWN")
        with mock.patch.object(xs8_gateway, "time", frozen):
            hold = self.hold()
            hold["started"], hold["browser_seen"] = 1000.0 - 20.0, 1000.0
            code, payload, _, _ = self.hold_request(payload["csrf_token"], cookie, {
                "action": "HEARTBEAT", "hold_id": hold["hold_id"]})
            self.assertEqual((code, payload["result"]), (409, "NOT_ACTIVE"))
            self.assertIn(payload.get("reason"), ("MAX_HOLD", None))
            self.assertIsNone(self.hold())

    def test_volume_and_cd_holds_exclude_each_other(self):
        self.gate(volume_hold_enabled=True, cd_seek_enabled=True, volume_step_enabled=True)
        fake = self.service()
        token, cookie = self.session()
        _, payload, _, _ = self.start_volume(token, cookie)
        token = payload["csrf_token"]
        # A CD seek, a volume step and a second hold are refused; nothing is sent.
        self.elapse_interval()
        code, payload, _, _ = self.seek(token, cookie, {"action": "START", "direction": "FORWARD"})
        self.assertEqual((code, payload["result"]), (409, "CD_BUSY"))
        self.spaced("volume_step")  # Shared with the hold START; spacing is not the point here.
        code, raw, _ = self.post("/api/v1/volume/step", payload["csrf_token"], cookie,
                                 json.dumps({"direction": "UP"}))
        self.assertEqual((code, json.loads(raw)["result"]), (409, "VOLUME_BUSY"))
        code, payload, _, _ = self.start_volume(self.fresh_token(cookie), cookie, "DOWN")
        self.assertEqual((code, payload["result"]), (409, "VOLUME_BUSY"))
        # A CD RELEASE (session-scoped) never ends the volume hold.
        code, payload, _, _ = self.seek(payload["csrf_token"], cookie, {"action": "RELEASE"})
        self.assertEqual((code, payload["result"]), (200, "NOT_ACTIVE"))
        self.assertIsNotNone(self.hold())
        self.assertEqual(fake.lines(), ["KEY_DOWN VOLUME_UP"])
        code, payload, _, _ = self.hold_request(payload["csrf_token"], cookie, {"action": "RELEASE"})
        self.assertEqual(payload["result"], "RELEASED")

    def test_volume_hold_uncertain_replies(self):
        self.gate(volume_hold_enabled=True)
        down = (b"ERR KEY_DOWN_UNCERTAIN action=VOLUME_UP state=FAULT_LATCHED stop_owed=0 "
                b"context_may_be_open=0 receiver_execution=UNVERIFIED\n")
        up = (b"ERR KEY_UP_UNCERTAIN action=VOLUME_UP state=FAULT_LATCHED stop_owed=1 "
              b"context_may_be_open=1 receiver_execution=UNVERIFIED\n")
        fake = self.service()
        fake.script["KEY_DOWN"] = [down]
        token, cookie = self.session()
        code, payload, _, _ = self.start_volume(token, cookie)
        self.assertEqual((code, payload["result"], payload["state"]),
                         (502, "SERVICE_UNCERTAIN", "FAULT_LATCHED"))
        self.assertTrue(payload["physical_check_required"])
        self.assertIsNone(self.hold())
        _, payload, _, _ = self.start_volume(payload["csrf_token"], cookie)
        fake.script["KEY_UP"] = [up]
        code, payload, _, _ = self.hold_request(payload["csrf_token"], cookie, {"action": "RELEASE"})
        self.assertEqual((code, payload["result"], payload["stop_owed"]),
                         (502, "SERVICE_UNCERTAIN", "1"))
        self.assertTrue(payload["physical_check_required"])
        self.assertEqual(sum(l.startswith("KEY_DOWN") for l in fake.lines()), 2)

    def test_cli_gates(self):
        base = ["--systemd-socket", "--nginx-uid", "33", "--https-origin",
                "https://woodland-pi.local"]
        for flag, name in (("--enable-volume-hold", "enable_volume_hold"),
                           ("--enable-power", "enable_power"),
                           ("--enable-tape2-monitor", "enable_tape2_monitor")):
            with self.subTest(flag=flag):
                self.assertTrue(getattr(xs8_gateway.parse_args([flag] + base), name))
                self.assertFalse(getattr(xs8_gateway.parse_args([]), name))
                for options in ([flag], [flag] + base + ["--socket", self.socket_path],
                                [flag, "--enable-offline-mute"] + base +
                                ["--socket", self.socket_path]):
                    with contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaises(SystemExit):
                            xs8_gateway.parse_args(options)


class RealServiceVolumeHoldTest(RealServiceSeekTest):
    """Web volume hold through the Gateway against the real service loop."""

    for _name in [n for n in dir(RealServiceSeekTest) if n.startswith("test_")]:
        locals()[_name] = None
    del _name

    def test_real_service_volume_hold_and_shutdown_release(self):
        handler = self.server.RequestHandlerClass
        handler.volume_hold_enabled = True
        handler.https_origin = "https://woodland-pi.local"
        self.server.proxy_peer_verified = True
        token, cookie = self.session()
        code, raw, _ = self.post("/api/v1/volume/hold", token, cookie,
                                 json.dumps({"action": "START", "direction": "DOWN"}))
        payload = json.loads(raw)
        self.assertEqual((code, payload["result"]), (200, "HOLD_STARTED"), payload)
        time.sleep(0.4)
        code, raw, _ = self.post("/api/v1/volume/hold", payload["csrf_token"], cookie,
                                 json.dumps({"action": "HEARTBEAT", "hold_id": payload["hold_id"]}))
        payload = json.loads(raw)
        self.assertEqual((code, payload["result"]), (200, "LEASE_RENEWED"), payload)
        self.assertEqual(self.transmitted(), ["0x17", "0xE9"])
        code, raw, _ = self.post("/api/v1/volume/hold", payload["csrf_token"], cookie,
                                 json.dumps({"action": "RELEASE"}))
        self.assertEqual((code, json.loads(raw)["result"]), (200, "RELEASED"), raw)
        self.assertEqual(self.transmitted(), ["0x17", "0xE9", "0x09", "0x07"])
        # A second hold, then Gateway shutdown: STOP then exit from the close.
        time.sleep(0.3)
        code, raw, _ = self.post("/api/v1/volume/hold", json.loads(raw)["csrf_token"], cookie,
                                 json.dumps({"action": "START", "direction": "UP"}))
        self.assertEqual(code, 200, raw)
        self.server.shutdown()
        self.server.server_close()
        self.wait_tx(["0x17", "0xE9", "0x09", "0x07", "0x17", "0xF9", "0x09", "0x07"])
        self.assertIn("XS8 STOP reason=CLIENT_DISCONNECT", self.log_path.read_text())


def receiver_status(source="CD", age_s=0.0, integrity="HEALTHY", base=None):
    """CD-capable STATUS with one receiver source observation, observed age_s ago."""
    observed = source != "UNKNOWN"
    ns = time.monotonic_ns() - int(age_s * 1e9) if observed else 0
    return (CD_IDLE_STATUS if base is None else base).replace(b"\n", (
        f" receiver_source={source} receiver_source_ns={ns}"
        f" receiver_source_evidence={'PASSIVE' if observed else 'NONE'}"
        " receiver_monitor=UNKNOWN receiver_monitor_ns=0 receiver_monitor_evidence=NONE"
        f" receiver_integrity={integrity}\n").encode())


CD_STOP_OK = CD_TRANSPORT_OK
SOURCE_UNCERTAIN = (b"ERR SOURCE_UNCERTAIN gpio_tx=TX_OK waveform=MISSING "
                    b"receiver_execution=UNVERIFIED\n")
CD_STOP_UNCERTAIN = (b"ERR CD_TRANSPORT_UNCERTAIN action=STOP gpio_tx=TX_OK "
                     b"waveform=MISSING receiver_execution=UNVERIFIED\n")


class ScriptedService(RecordingService):
    """Per-command replies (None: close without replying) and optional delays.
    Each connection is answered on its own thread, so STATUS is never queued
    behind a delayed command."""

    def __init__(self, path, status, replies, delays=None):
        self.replies = replies
        self.delays = delays or {}
        super().__init__(path, status=status)

    def run(self):
        while not self.stop.is_set():
            try:
                client, _ = self.listener.accept()
            except socket.timeout:
                continue
            threading.Thread(target=self.answer, args=(client,), daemon=True).start()

    def answer(self, client):
        with client:
            command = client.recv(128)
            self.commands.append(command)
            time.sleep(self.delays.get(command, 0))
            reply = self.status if command == b"STATUS\n" else self.replies.get(command)
            if reply is not None:
                try:
                    client.sendall(reply)
                except BrokenPipeError:
                    pass


class SourceCdStopGatewayTest(GatewayTest):
    """Leaving a fresh CD source: SELECT_SOURCE, then CD_STOP, in the Gateway."""

    for _name in [n for n in dir(GatewayTest) if n.startswith("test_")]:
        locals()[_name] = None
    del _name

    def setUp(self):
        super().setUp()
        handler = self.server.RequestHandlerClass
        handler.production_source_enabled = True
        handler.https_origin = "https://woodland-pi.local"
        self.server.proxy_peer_verified = True
        self.fake = None

    def tearDown(self):
        if self.fake is not None:
            self.fake.close()
        super().tearDown()

    def service(self, status, stop=CD_STOP_OK, source=SOURCE_OK, delays=None):
        if self.fake is not None:
            self.fake.close()
        replies = {b"CD_STOP\n": stop, b"CD_PLAY_PAUSE\n": CD_TRANSPORT_OK,
                   b"VOLUME_STEP UP\n": None, b"TAPE2_MONITOR_TOGGLE\n": MONITOR_OK}
        for name in xs8_gateway.EVIDENCED_SOURCES:
            replies[f"SELECT_SOURCE {name}\n".encode()] = source
        self.fake = ScriptedService(self.socket_path, status, replies, delays)
        return self.fake

    def select(self, name, token=None, cookie=None):
        if token is None:
            token, cookie = self.session()  # A new session: no per-session spacing.
        code, body, _ = self.source(token, cookie, name)
        return code, json.loads(body)

    def sent(self):
        return [c for c in self.fake.commands if c != b"STATUS\n"]

    def elapse_cd_interval(self):
        state = self.server.__dict__.get("cd_track")
        if state:
            state["completed"] = float("-inf")

    def fresh_token(self, cookie):
        code, body, _ = self.request_full("GET", "/api/v1/session", headers={
            "X-Kenwood-Authenticated-User": "remote", "Cookie": cookie})
        self.assertEqual(code, 200)
        return json.loads(body)["csrf_token"]

    def test_leaving_fresh_cd_selects_then_stops_for_every_other_source(self):
        self.assertEqual(xs8_gateway.SOURCES_STOPPING_CD, ("TUNER", "PHONO", "VIDEO1", "VIDEO2", "TAPE1"))
        for name in xs8_gateway.SOURCES_STOPPING_CD:
            with self.subTest(source=name):
                self.elapse_cd_interval()
                self.service(receiver_status("CD"))
                code, payload = self.select(name)
                self.assertEqual(code, 200, payload)
                select = f"SELECT_SOURCE {name}\n".encode()
                self.assertEqual(self.sent(), [select, b"CD_STOP\n"])
                # Directly after the source: no STATUS read or other request between.
                commands = self.fake.commands
                self.assertEqual(commands[commands.index(select) + 1], b"CD_STOP\n")
                self.assertEqual((payload["result"], payload["source"], payload["gpio_tx"],
                                  payload["waveform"], payload["receiver_execution"]),
                                 ("SERVICE_TX_OK", name, "TX_OK", "MATCH", "UNVERIFIED"))
                self.assertEqual(payload["cd_stop"], {"gpio_tx": "TX_OK", "waveform": "MATCH",
                                                      "receiver_execution": "UNVERIFIED"})

    def test_no_stop_for_cd_other_sources_unknown_stale_or_unreliable(self):
        cases = [("CD", receiver_status("CD")),                           # CD -> CD
                 ("VIDEO1", receiver_status("TUNER")),                       # non-CD -> other
                 ("CD", receiver_status("TUNER")),
                 ("VIDEO2", receiver_status("UNKNOWN")),
                 ("VIDEO1", receiver_status("CD", age_s=31)),                # LAST_KNOWN
                 ("TUNER", receiver_status("CD", integrity="UNCERTAIN")),
                 ("PHONO", CD_IDLE_STATUS)]                               # no observation
        for name, status in cases:
            with self.subTest(source=name, status=status[-120:]):
                self.elapse_cd_interval()
                self.service(status)
                code, payload = self.select(name)
                self.assertEqual(code, 200, payload)
                self.assertEqual(self.sent(), [f"SELECT_SOURCE {name}\n".encode()])
                self.assertNotIn("cd_stop", payload)

    def test_video_names_are_canonical_in_the_api(self):
        self.service(receiver_status("CD"))
        for name in ("TV1", "TV2", "video1"):
            with self.subTest(name=name):
                self.assertEqual(self.select(name)[0], 400)
        self.assertEqual(self.sent(), [])
        code, body, _ = self.request_full("GET", "/api/v1/capabilities", headers={
            "X-Kenwood-Authenticated-User": "remote"})
        sources = json.loads(body)["operations"]["source"]["available_sources"]
        self.assertEqual(sources, ["CD", "TUNER", "PHONO", "VIDEO1", "VIDEO2", "TAPE1"])
        self.service(receiver_status("VIDEO2"))
        code, body, _ = self.request_full("GET", "/api/v1/status", headers={
            "X-Kenwood-Authenticated-User": "remote"})
        self.assertEqual(json.loads(body)["receiver"]["source"]["value"], "VIDEO2")

    def test_tape2_and_monitor_never_stop_the_cd(self):
        self.server.RequestHandlerClass.tape2_monitor_enabled = True
        self.service(receiver_status("CD"))
        code, payload = self.select("TAPE2")
        self.assertEqual((code, payload["result"]), (409, "SOURCE_UNVERIFIED"))
        token, cookie = self.session()
        code, raw, _ = self.request_full("POST", "/api/v1/receiver/monitor", body="{}", headers={
            "Origin": "https://woodland-pi.local", "X-Forwarded-Proto": "https",
            "X-Kenwood-Authenticated-User": "remote", "X-CSRF-Token": token,
            "Cookie": cookie, "Content-Type": "application/json"})
        self.assertEqual(code, 200, raw)
        self.assertEqual(self.sent(), [b"TAPE2_MONITOR_TOGGLE\n"])

    def test_source_failure_or_timeout_never_stops(self):
        for reply, code, result, physical in (
                (b"ERR VOLUME_BUSY\n", 409, "REJECTED", False),
                (SOURCE_UNCERTAIN, 502, "SERVICE_UNCERTAIN", True),
                (b"OK SOMETHING_ELSE\n", 502, "INVALID_REPLY", True),
                (None, 502, "DISCONNECTED", True)):
            with self.subTest(result=result):
                self.elapse_cd_interval()
                self.service(receiver_status("CD"), source=reply)
                status, payload = self.select("VIDEO1")
                self.assertEqual((status, payload["result"]), (code, result), payload)
                self.assertEqual(self.sent(), [b"SELECT_SOURCE VIDEO1\n"],
                                 "never CD_STOP, never a retry")
                self.assertEqual((payload["stage"], payload["source"], payload["cd_stop_sent"],
                                  payload["physical_check_required"], payload["receiver_execution"]),
                                 ("SELECT_SOURCE", "VIDEO1", False, physical, "UNVERIFIED"))
                self.assertNotIn("cd_stop", payload)
                if reply is SOURCE_UNCERTAIN:
                    self.assertEqual((payload["gpio_tx"], payload["waveform"]), ("TX_OK", "MISSING"))
        # A reply timeout is uncertain: no CD_STOP either.
        self.elapse_cd_interval()
        self.service(receiver_status("CD"))
        with mock.patch.object(xs8_gateway, "send_source", side_effect=
                               xs8_gateway.MuteServiceError("TIMEOUT", "timed out")) as select, \
                mock.patch.object(xs8_gateway, "send_cd_playback") as stop:
            status, payload = self.select("TUNER")
        self.assertEqual((status, payload["result"], payload["stage"], payload["cd_stop_sent"],
                          payload["physical_check_required"]),
                         (504, "TIMEOUT", "SELECT_SOURCE", False, True))
        select.assert_called_once()
        stop.assert_not_called()

    def test_stop_failure_after_select_requires_physical_check(self):
        for reply, code, result in (
                (CD_STOP_UNCERTAIN, 502, "SERVICE_UNCERTAIN"),
                (b"ERR CD_TRACK_BUSY\n", 409, "REJECTED"),
                (b"ERR CD_FAULT_LATCHED\n", 409, "CD_FAULT_LATCHED"),
                (b"OK SOMETHING_ELSE\n", 502, "INVALID_REPLY"),
                (None, 502, "DISCONNECTED")):
            with self.subTest(result=result):
                self.elapse_cd_interval()
                self.service(receiver_status("CD"), stop=reply)
                status, payload = self.select("VIDEO2")
                self.assertEqual((status, payload["result"]), (code, result), payload)
                self.assertEqual(self.sent(), [b"SELECT_SOURCE VIDEO2\n", b"CD_STOP\n"],
                                 "never a retry")
                # Even a clean refusal: the receiver switched, the changer may play.
                self.assertEqual((payload["stage"], payload["source"], payload["cd_stop_sent"],
                                  payload["physical_check_required"], payload["receiver_execution"]),
                                 ("CD_STOP", "VIDEO2", True, True, "UNVERIFIED"))
                # The top-level evidence is the verified source; the STOP's is its own.
                self.assertEqual((payload["gpio_tx"], payload["waveform"]), ("TX_OK", "MATCH"))
                self.assertEqual(payload["cd_stop"]["result"], result)
                if reply is CD_STOP_UNCERTAIN:
                    self.assertEqual(payload["cd_stop"], {"result": result, "gpio_tx": "TX_OK",
                        "waveform": "MISSING", "receiver_execution": "UNVERIFIED"})
        self.elapse_cd_interval()
        self.service(receiver_status("CD"))
        with mock.patch.object(xs8_gateway, "send_cd_playback", side_effect=
                               xs8_gateway.MuteServiceError("TIMEOUT", "timed out")) as stop:
            status, payload = self.select("PHONO")
        self.assertEqual((status, payload["result"], payload["stage"], payload["cd_stop"],
                          payload["physical_check_required"]),
                         (504, "TIMEOUT", "CD_STOP", {"result": "TIMEOUT"}, True))
        stop.assert_called_once()
        self.assertEqual(self.sent(), [b"SELECT_SOURCE PHONO\n"])

    def test_hold_fault_spacing_and_cd_field_gates_send_nothing(self):
        holding = receiver_status("CD", base=CD_HOLD_STATUS)
        volume = receiver_status("CD").replace(b"OK state=IDLE", b"OK state=HOLDING_UP")
        fault = receiver_status("CD", base=CD_FAULT_STATUS)
        no_cd_fields = receiver_status("CD", base=STATUS)
        for status, code, result in ((holding, 409, "NOT_ELIGIBLE"), (volume, 409, "NOT_ELIGIBLE"),
                                     (fault, 409, "NOT_ELIGIBLE"),
                                     (no_cd_fields, 409, "NOT_ELIGIBLE")):
            with self.subTest(status=status[:60]):
                self.service(status)
                self.assertEqual(self.select("VIDEO1"), (code, {"result": result,
                    "receiver_execution": "UNVERIFIED", **({"source": "VIDEO1"}
                    if status is no_cd_fields else {})}))
                self.assertEqual(self.sent(), [])
        # The STOP is a CD dispatch: inside the CD interval it is refused, unsent.
        self.service(receiver_status("CD"))
        self.server.__dict__.setdefault("cd_track", {"lock": threading.Lock(),
                                                     "completed": 0.0})["completed"] = time.monotonic()
        code, payload = self.select("VIDEO1")
        self.assertEqual((code, payload["result"]), (429, "TOO_FAST"))
        self.assertEqual(self.sent(), [])

    def test_queued_request_runs_only_after_stop_and_select(self):
        # The Gateway serves one request at a time: a request arriving during
        # CD_STOP waits until the whole pair has completed.
        self.service(receiver_status("CD"), delays={b"CD_STOP\n": 0.6})
        results = {}
        first = threading.Thread(target=lambda: results.setdefault("first", self.select("VIDEO1")))
        first.start()
        deadline = time.monotonic() + 2
        while b"CD_STOP\n" not in self.fake.commands:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        token, cookie = self.session()
        self.assertEqual(self.select("CD", token, cookie)[0], 200)
        first.join(timeout=5)
        self.assertEqual(results["first"][0], 200, results)
        self.assertEqual(self.sent(), [b"SELECT_SOURCE VIDEO1\n", b"CD_STOP\n", b"SELECT_SOURCE CD\n"])

    def test_control_locks_refuse_requests_during_stop_and_select(self):
        # Defense in depth for a concurrent server: the source request holds every
        # control lock from before STATUS until CD_STOP has answered.
        self.server.shutdown()
        self.thread.join(timeout=2)
        self.server.server_close()
        threaded = type("ThreadedGateway", (socketserver.ThreadingMixIn,
                                            xs8_gateway.GatewayHTTPServer),
                        {"daemon_threads": True})
        handler = self.server.RequestHandlerClass
        handler.cd_playback_enabled = True
        handler.volume_step_enabled = True
        handler.power_enabled = True
        self.server = threaded(("127.0.0.1", 0), handler)
        self.server.proxy_peer_verified = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.service(receiver_status("CD"), delays={b"CD_STOP\n": 1.5})
        results = {}
        first = threading.Thread(target=lambda: results.setdefault("first", self.select("VIDEO1")))
        first.start()
        deadline = time.monotonic() + 2
        while b"CD_STOP\n" not in self.fake.commands:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        token, cookie = self.session()
        during = time.monotonic()

        def post(path, body, session_cookie=None):
            session_cookie = session_cookie or cookie
            token = self.fresh_token(session_cookie)  # Every refusal consumes the token.
            code, raw, _ = self.request_full("POST", path, body=body, headers={
                "Origin": "https://woodland-pi.local", "X-Forwarded-Proto": "https",
                "X-Kenwood-Authenticated-User": "remote", "X-CSRF-Token": token,
                "Cookie": session_cookie, "Content-Type": "application/json"})
            return code, json.loads(raw)["result"]
        self.assertEqual(self.select("CD", token, cookie)[1]["result"], "BUSY")
        # Another session (no per-session spacing) is refused the same way.
        self.assertEqual(post("/api/v1/source", '{"source":"TUNER"}', self.session()[1]),
                         (409, "BUSY"))
        self.assertEqual(post("/api/v1/cd/playback", '{"action":"PLAY_PAUSE"}'), (409, "CD_BUSY"))
        self.assertEqual(post("/api/v1/volume/step", '{"direction":"UP"}'), (409, "VOLUME_BUSY"))
        self.assertEqual(post("/api/v1/power", '{"action":"ON"}'), (409, "BUSY"))
        self.assertLess(time.monotonic() - during, 1.0, "all refused while CD_STOP was in flight")
        first.join(timeout=5)
        self.assertEqual(results["first"][0], 200, results)
        self.assertEqual(self.sent(), [b"SELECT_SOURCE VIDEO1\n", b"CD_STOP\n"])
        # The locks are released: the next request is served normally.
        self.elapse_cd_interval()
        self.assertEqual(post("/api/v1/source", '{"source":"VIDEO2"}', self.session()[1])[0], 200)
        self.assertEqual(self.sent(), [b"SELECT_SOURCE VIDEO1\n", b"CD_STOP\n",
                                       b"SELECT_SOURCE VIDEO2\n", b"CD_STOP\n"])

    def test_csrf_single_use_and_recovery_without_duplicates(self):
        self.service(receiver_status("CD"), stop=CD_STOP_UNCERTAIN)
        token, cookie = self.session()
        code, payload = self.select("VIDEO1", token, cookie)
        self.assertEqual((code, payload["stage"]), (502, "CD_STOP"))
        # The consumed token never dispatches again.
        self.assertEqual(self.select("VIDEO1", token, cookie)[0], 403)
        self.assertEqual(self.sent(), [b"SELECT_SOURCE VIDEO1\n", b"CD_STOP\n"])
        # A recovered token is a new, explicit request: one STOP, one SELECT.
        time.sleep(1.0)  # The existing per-session source spacing.
        self.elapse_cd_interval()
        self.service(receiver_status("CD"))
        token = self.fresh_token(cookie)
        code, payload = self.select("VIDEO1", token, cookie)
        self.assertEqual(code, 200, payload)
        self.assertEqual(self.sent(), [b"SELECT_SOURCE VIDEO1\n", b"CD_STOP\n"])
        self.assertEqual(self.select("VIDEO1", token, cookie)[0], 403)
        self.assertEqual(len(self.sent()), 2)


class BindingTest(unittest.TestCase):
    def parse_error(self, *args):
        with contextlib.redirect_stderr(io.StringIO()) as output:
            with self.assertRaises(SystemExit) as raised:
                xs8_gateway.parse_args(list(args))
        self.assertEqual(raised.exception.code, 2)
        return output.getvalue()

    def test_default_loopback_and_port(self):
        args = xs8_gateway.parse_args([])
        self.assertEqual(args.bind, "127.0.0.1")
        self.assertEqual(args.port, 8080)
        self.assertFalse(args.allow_insecure_readonly_lan)
        self.assertFalse(args.enable_dev_mute)

    def test_legacy_dev_gate_remains_disabled(self):
        for options in ([], ["--systemd-socket", "--nginx-uid", "1000"]):
            self.assertIn("disabled in phase 2A", self.parse_error(
                *options, "--enable-dev-mute", "--https-origin",
                "https://woodland-pi.local", "--socket", "/tmp/fake-xs8.sock"))

    def test_offline_gate_requires_activated_socket_fake_path_and_https_origin(self):
        base = ["--systemd-socket", "--nginx-uid", "33", "--enable-offline-mute"]
        fake = ["--socket", "/tmp/offline-fake-xs8.sock"]
        origin = ["--https-origin", "https://woodland-pi.local"]
        args = xs8_gateway.parse_args(base + fake + origin)
        self.assertTrue(args.enable_offline_mute)
        for options in (base + origin, base + fake, fake + origin + ["--enable-offline-mute"],
                        base + fake + origin + ["--bind", "127.0.0.1"],
                        base + fake + origin + ["--allow-insecure-readonly-lan"],
                        base + fake + ["--https-origin", "http://woodland-pi.local"],
                        base + fake + ["--https-origin", "https://woodland-pi.local/path"],
                        base + fake + ["--https-origin", "https://user@woodland-pi.local"]):
            with self.subTest(options=options):
                self.parse_error(*options)
        with tempfile.TemporaryDirectory() as directory:
            real = Path(directory) / "real.sock"
            alias = Path(directory) / "alias.sock"
            with mock.patch.object(xs8_gateway, "DEFAULT_SOCKET", str(real)):
                alias.symlink_to(real)
                self.parse_error(*(base + ["--socket", str(alias)] + origin))

    def test_production_gate_requires_real_socket_and_activated_https_proxy(self):
        base = ["--systemd-socket", "--nginx-uid", "33", "--enable-mute",
                "--https-origin", "https://woodland-pi.local"]
        args = xs8_gateway.parse_args(base)
        self.assertTrue(args.enable_mute)
        self.assertEqual(args.socket, xs8_gateway.DEFAULT_SOCKET)
        for options in (base + ["--socket", "/tmp/fake-xs8.sock"],
                        base + ["--bind", "127.0.0.1"],
                        base + ["--allow-insecure-readonly-lan"],
                        base + ["--enable-offline-mute"],
                        ["--enable-mute", "--https-origin", "https://woodland-pi.local"],
                        ["--systemd-socket", "--nginx-uid", "33", "--enable-mute"]):
            with self.subTest(options=options):
                self.parse_error(*options)

    def test_deployment_templates_keep_auth_and_control_gate_off(self):
        root = Path(xs8_gateway.__file__).resolve().parent
        nginx = (root / "deploy/nginx/kenwood-xs-remote.conf.example").read_text()
        service = (root / "deploy/systemd/kenwood-xs-gateway.service.example").read_text()
        listener = (root / "deploy/systemd/kenwood-xs-gateway.socket.example").read_text()
        self.assertIn('listen __LAN_IPV4__:443 ssl;', nginx)
        self.assertIn('auth_basic "Kenwood XS Remote";', nginx)
        self.assertIn('auth_basic_user_file __PASSWORD_FILE_PATH__;', nginx)
        self.assertIn('proxy_set_header X-Kenwood-Authenticated-User $remote_user;', nginx)
        self.assertIn('proxy_set_header Authorization "";', nginx)
        self.assertIn('proxy_pass http://unix:/run/kenwood-gateway.sock:;', nginx)
        self.assertIn('--systemd-socket --nginx-uid __NGINX_WORKER_UID__', service)
        self.assertNotIn('--enable-dev-mute', service)
        self.assertNotIn('--enable-offline-mute', service)
        self.assertIn('ListenStream=/run/kenwood-gateway.sock', listener)
        self.assertIn('SocketMode=0660', listener)

    def test_systemd_socket_options(self):
        args = xs8_gateway.parse_args(["--systemd-socket", "--nginx-uid", "33"])
        self.assertTrue(args.systemd_socket)
        self.assertEqual(args.nginx_uid, 33)
        for options in (["--systemd-socket"], ["--nginx-uid", "33"],
                        ["--systemd-socket", "--nginx-uid", "0"],
                        ["--systemd-socket", "--nginx-uid", "33", "--port", "8080"],
                        ["--systemd-socket", "--nginx-uid", "33", "--bind", "127.0.0.1"],
                        ["--systemd-socket", "--nginx-uid", "33", "--allow-insecure-readonly-lan"]):
            self.parse_error(*options)

    def test_systemd_listener_rejects_missing_or_wrong_descriptor(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, "exactly one"):
                xs8_gateway.systemd_listener()
        inherited = mock.Mock(family=socket.AF_INET, type=socket.SOCK_STREAM)
        inherited.getsockname.return_value = xs8_gateway.GATEWAY_SOCKET
        inherited.getsockopt.return_value = 1
        env = {"LISTEN_PID": str(os.getpid()), "LISTEN_FDS": "1"}
        with mock.patch.dict(os.environ, env), \
             mock.patch.object(xs8_gateway.socket, "socket", return_value=inherited):
            with self.assertRaisesRegex(ValueError, "unexpected"):
                xs8_gateway.systemd_listener()
        inherited.close.assert_called_once()

        inherited = mock.Mock(family=socket.AF_UNIX, type=socket.SOCK_STREAM)
        inherited.getsockname.return_value = xs8_gateway.GATEWAY_SOCKET
        inherited.getsockopt.return_value = 1
        with mock.patch.dict(os.environ, env), \
             mock.patch.object(xs8_gateway.socket, "socket", return_value=inherited) as factory:
            self.assertIs(xs8_gateway.systemd_listener(), inherited)
        factory.assert_called_once_with(fileno=3)

    def test_non_loopback_requires_opt_in(self):
        self.assertIn("requires --allow-insecure-readonly-lan",
                      self.parse_error("--bind", "192.168.1.20"))

    def test_specific_lan_address_with_opt_in(self):
        args = xs8_gateway.parse_args([
            "--bind", "192.168.1.20", "--allow-insecure-readonly-lan"])
        self.assertEqual(args.bind, "192.168.1.20")
        self.assertTrue(args.allow_insecure_readonly_lan)

    def test_lan_startup_uses_requested_address_and_warns(self):
        server = mock.Mock(server_port=8080)
        with mock.patch.object(xs8_gateway, "GatewayHTTPServer", return_value=server) as factory:
            with contextlib.redirect_stderr(io.StringIO()) as warning:
                with contextlib.redirect_stdout(io.StringIO()):
                    xs8_gateway.main([
                        "--bind", "192.168.1.20",
                        "--allow-insecure-readonly-lan"])
        self.assertEqual(factory.call_args.args[0], ("192.168.1.20", 8080))
        self.assertIn("unauthenticated, unencrypted", warning.getvalue())
        server.serve_forever.assert_called_once()
        server.server_close.assert_called_once()

    def test_wildcards_rejected_even_with_opt_in(self):
        for address in ("0.0.0.0", "::"):
            self.assertIn("wildcard binding is not allowed",
                          self.parse_error("--bind", address,
                                           "--allow-insecure-readonly-lan"))

    def test_opt_in_without_lan_address_is_rejected(self):
        self.assertIn("requires a non-loopback",
                      self.parse_error("--allow-insecure-readonly-lan"))

    def test_hostnames_and_ipv6_are_rejected(self):
        for address in ("localhost", "::1"):
            self.assertIn("literal IPv4 address",
                          self.parse_error("--bind", address))


class ActivatedGatewayTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = str(Path(self.temp.name) / "gateway.sock")
        self.service_path = str(Path(self.temp.name) / "service.sock")
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(self.path)
        self.listener.listen(5)
        self.start_gateway()

    def start_gateway(self, dev_mute=False, production_mute=False):
        handler = type("UnixTestHandler", (xs8_gateway.GatewayHandler,),
                       {"socket_path": self.service_path, "dev_mute_enabled": dev_mute,
                        "production_mute_enabled": production_mute,
                        "https_origin": "https://woodland-pi.local", "sessions": {},
                        "log_message": lambda *args: None})
        self.server = xs8_gateway.ActivatedUnixServer(
            self.listener.dup(), handler, os.getuid())
        # Exercise actual Linux SO_PEERCRED; macOS lacks it.
        if not hasattr(socket, "SO_PEERCRED"):
            self.server.verify_request = lambda request, address: True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop_gateway(self):
        self.server.shutdown()
        self.thread.join(timeout=2)
        self.server.server_close()

    def tearDown(self):
        self.stop_gateway()
        self.listener.close()
        self.temp.cleanup()

    def request(self, method, path, headers=None, body=None):
        payload = body.encode() if isinstance(body, str) else (body or b"")
        fields = {"Host": "localhost", **(headers or {})}
        if body is not None and "Content-Length" not in fields:
            fields["Content-Length"] = str(len(payload))
        request = (f"{method} {path} HTTP/1.1\r\n" +
                   "".join(f"{name}: {value}\r\n" for name, value in fields.items()) +
                   "Connection: close\r\n\r\n").encode() + payload
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(2)
            client.connect(self.path)
            client.sendall(request)
            response = http.client.HTTPResponse(client)
            response.begin()
            return response.status, response.read(), dict(response.getheaders())

    def nginx_like_request(self, method, path, credentials, headers=None, body=None):
        """Model Basic Auth and nginx's header replacement for offline tests."""
        if credentials != "correct-test-password":
            return 401, b"", {}
        upstream = {key: value for key, value in (headers or {}).items()
                    if key.lower() not in ("x-kenwood-authenticated-user", "authorization",
                                           "x-forwarded-proto")}
        upstream["X-Kenwood-Authenticated-User"] = "remote"
        upstream["X-Forwarded-Proto"] = "https"
        return self.request(method, path, upstream, body)

    def test_offline_mute_uses_activated_socket_and_fake_challenge(self):
        self.stop_gateway()
        self.start_gateway(dev_mute=True)
        forged = {"X-Kenwood-Authenticated-User": "attacker"}
        self.assertEqual(self.nginx_like_request(
            "GET", "/api/v1/session", None, forged)[0], 401)
        code, body, response_headers = self.nginx_like_request(
            "GET", "/api/v1/session", "correct-test-password", forged)
        self.assertEqual(code, 200)
        token = json.loads(body)["csrf_token"]
        cookie = response_headers["Set-Cookie"].split(";", 1)[0]
        self.assertEqual(self.server.RequestHandlerClass.sessions[cookie.split("=", 1)[1]]["user"],
                         "remote")
        headers = {**forged, "Origin": "https://woodland-pi.local",
                   "Cookie": cookie, "X-CSRF-Token": token,
                   "Content-Type": "application/json"}
        for credentials, changes, expected in (
                (None, {}, 401),
                ("wrong", {}, 401),
                ("correct-test-password", {"Origin": "https://wrong.example"}, 403),
                ("correct-test-password", {"X-CSRF-Token": "wrong"}, 403),
                ("correct-test-password", {"X-CSRF-Token": ""}, 403),
                ("correct-test-password", {"Content-Type": "text/plain"}, 400)):
            with self.subTest(credentials=credentials, changes=changes):
                self.assertEqual(self.nginx_like_request(
                    "POST", "/api/v1/mute", credentials,
                    {**headers, **changes}, "{}")[0], expected)
        missing_token = {key: value for key, value in headers.items() if key != "X-CSRF-Token"}
        self.assertEqual(self.nginx_like_request(
            "POST", "/api/v1/mute", "correct-test-password", missing_token, "{}")[0], 403)
        self.assertEqual(self.nginx_like_request(
            "POST", "/api/v1/mute", "correct-test-password", headers, '{"x":1}')[0], 400)
        fake = OfflineFakeService(self.service_path, b"OK MUTE_TX\n")
        code, body, _ = self.nginx_like_request(
            "POST", "/api/v1/mute", "correct-test-password", headers, "{}")
        fake.close()
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)["receiver_execution"], "UNVERIFIED")
        self.assertEqual(len([line for line in fake.commands if line == b"MUTE\n"]), 1)
        self.assertEqual(len(fake.commands), 2)
        self.assertTrue(fake.commands[0].startswith(b"OFFLINE_TEST "))
        self.assertEqual(self.nginx_like_request(
            "POST", "/api/v1/mute", "correct-test-password", headers, "{}")[0], 403)
        for path in ("/api/v1/source", "/api/v1/volume/step", "/api/v1/cd/transport"):
            self.assertEqual(self.nginx_like_request(
                "POST", path, "correct-test-password", headers, "{}")[0], 501)

    def test_offline_fake_gate_rejects_real_service_and_unavailable_socket(self):
        self.stop_gateway()
        self.start_gateway(dev_mute=True)
        code, body, response_headers = self.nginx_like_request(
            "GET", "/api/v1/session", "correct-test-password")
        self.assertEqual(code, 200)
        headers = {"Origin": "https://woodland-pi.local",
                   "Cookie": response_headers["Set-Cookie"].split(";", 1)[0],
                   "X-CSRF-Token": json.loads(body)["csrf_token"],
                   "Content-Type": "application/json"}
        code, body, _ = self.nginx_like_request(
            "POST", "/api/v1/mute", "correct-test-password", headers, "{}")
        self.assertEqual(code, 503)
        self.assertEqual(json.loads(body)["result"], "UNAVAILABLE")
        code, body, response_headers = self.nginx_like_request(
            "GET", "/api/v1/session", "correct-test-password", {"Cookie": headers["Cookie"]})
        headers["X-CSRF-Token"] = json.loads(body)["csrf_token"]
        fake = FakeService(self.service_path, b"ERR UNKNOWN_COMMAND\n")
        code, body, _ = self.nginx_like_request(
            "POST", "/api/v1/mute", "correct-test-password", headers, "{}")
        fake.close()
        self.assertEqual(code, 502)
        self.assertEqual(json.loads(body)["result"], "UNSAFE_SERVICE")
        self.assertEqual(len([line for line in fake.commands if b"MUTE" in line]), 0)

    def test_offline_mute_preserves_service_outcome_classes(self):
        self.stop_gateway()
        self.start_gateway(dev_mute=True)
        for service_reply, delay, expected_code, expected_result in (
                (b"ERR MONITOR_FAILED\n", 0, 409, "REJECTED"),
                (b"ERR MUTE_UNCERTAIN\n", 0, 502, "SERVICE_UNCERTAIN"),
                (None, 0, 502, "DISCONNECTED"),
                (None, 1.2, 504, "TIMEOUT")):
            with self.subTest(expected_result=expected_result):
                code, body, response_headers = self.nginx_like_request(
                    "GET", "/api/v1/session", "correct-test-password")
                self.assertEqual(code, 200)
                headers = {"Origin": "https://woodland-pi.local",
                           "Cookie": response_headers["Set-Cookie"].split(";", 1)[0],
                           "X-CSRF-Token": json.loads(body)["csrf_token"],
                           "Content-Type": "application/json"}
                fake = OfflineFakeService(self.service_path, service_reply, delay)
                code, body, _ = self.nginx_like_request(
                    "POST", "/api/v1/mute", "correct-test-password", headers, "{}")
                fake.close()
                self.assertEqual(code, expected_code)
                self.assertEqual(json.loads(body)["result"], expected_result)
                self.assertEqual(len([line for line in fake.commands if line == b"MUTE\n"]), 1)

    def test_production_gate_dispatches_one_mute_after_fresh_eligible_status(self):
        self.stop_gateway()
        self.start_gateway(production_mute=True)
        fake = RecordingService(self.service_path)
        try:
            code, body, response_headers = self.nginx_like_request(
                "GET", "/api/v1/session", "correct-test-password")
            self.assertEqual(code, 200)
            self.assertTrue(json.loads(body)["mute_available"])
            headers = {"Origin": "https://woodland-pi.local",
                       "Cookie": response_headers["Set-Cookie"].split(";", 1)[0],
                       "X-CSRF-Token": json.loads(body)["csrf_token"],
                       "Content-Type": "application/json"}
            self.assertEqual(fake.commands, [b"STATUS\n"])
            for credentials, changes, payload, expected in (
                    (None, {}, "{}", 401),
                    ("wrong", {}, "{}", 401),
                    ("correct-test-password", {"Origin": "https://wrong.example"}, "{}", 403),
                    ("correct-test-password", {"X-CSRF-Token": "wrong"}, "{}", 403),
                    ("correct-test-password", {}, '{"x":1}', 400)):
                self.assertEqual(self.nginx_like_request(
                    "POST", "/api/v1/mute", credentials,
                    {**headers, **changes}, payload)[0], expected)
            self.assertEqual(fake.commands, [b"STATUS\n"])
            code, body, _ = self.nginx_like_request(
                "POST", "/api/v1/mute", "correct-test-password", headers, "{}")
            self.assertEqual(code, 200)
            self.assertEqual(json.loads(body)["result"], "SERVICE_TX_OK")
            self.assertEqual(json.loads(body)["receiver_execution"], "UNVERIFIED")
            self.assertEqual(fake.commands, [b"STATUS\n", b"STATUS\n", b"MUTE\n"])
            self.assertEqual(self.nginx_like_request(
                "POST", "/api/v1/mute", "correct-test-password", headers, "{}")[0], 403)
            for path in ("/api/v1/source", "/api/v1/volume/step", "/api/v1/cd/transport"):
                self.assertEqual(self.nginx_like_request(
                    "POST", path, "correct-test-password", headers, "{}")[0], 501)
            self.assertEqual(fake.commands.count(b"MUTE\n"), 1)
        finally:
            fake.close()

    def test_production_gate_rejects_inhibited_faulted_and_unavailable_service(self):
        self.stop_gateway()
        self.start_gateway(production_mute=True)
        for status in (STATUS.replace(b"state=IDLE", b"state=BOOT_INHIBIT"),
                       STATUS.replace(b"state=IDLE", b"state=FAULT_LATCHED"),
                       STATUS.replace(b"monitor=HEALTHY", b"monitor=FAILED"),
                       STATUS.replace(b"stop_owed=0", b"stop_owed=1"),
                       STATUS.replace(b"context_may_be_open=0", b"context_may_be_open=1"),
                       CD_HOLD_STATUS):
            with self.subTest(status=status):
                fake = RecordingService(self.service_path, status=status)
                try:
                    code, body, response_headers = self.nginx_like_request(
                        "GET", "/api/v1/session", "correct-test-password")
                    self.assertEqual(code, 200)
                    self.assertFalse(json.loads(body)["mute_available"])
                    headers = {"Origin": "https://woodland-pi.local",
                               "Cookie": response_headers["Set-Cookie"].split(";", 1)[0],
                               "X-CSRF-Token": json.loads(body)["csrf_token"],
                               "Content-Type": "application/json"}
                    code, body, _ = self.nginx_like_request(
                        "POST", "/api/v1/mute", "correct-test-password", headers, "{}")
                    self.assertEqual(code, 409)
                    self.assertEqual(json.loads(body)["result"], "NOT_ELIGIBLE")
                    self.assertEqual(fake.commands, [b"STATUS\n", b"STATUS\n"])
                finally:
                    fake.close()
        code, body, response_headers = self.nginx_like_request(
            "GET", "/api/v1/session", "correct-test-password")
        self.assertEqual(code, 200)
        self.assertFalse(json.loads(body)["mute_available"])
        headers["Cookie"] = response_headers["Set-Cookie"].split(";", 1)[0]
        headers["X-CSRF-Token"] = json.loads(body)["csrf_token"]
        code, body, _ = self.nginx_like_request(
            "POST", "/api/v1/mute", "correct-test-password", headers, "{}")
        self.assertEqual(code, 503)
        self.assertEqual(json.loads(body)["result"], "UNAVAILABLE")

    def test_production_uncertain_reply_has_no_retry(self):
        self.stop_gateway()
        self.start_gateway(production_mute=True)
        fake = RecordingService(self.service_path, mute_reply=b"ERR MUTE_UNCERTAIN\n")
        try:
            code, body, response_headers = self.nginx_like_request(
                "GET", "/api/v1/session", "correct-test-password")
            headers = {"Origin": "https://woodland-pi.local",
                       "Cookie": response_headers["Set-Cookie"].split(";", 1)[0],
                       "X-CSRF-Token": json.loads(body)["csrf_token"],
                       "Content-Type": "application/json"}
            code, body, _ = self.nginx_like_request(
                "POST", "/api/v1/mute", "correct-test-password", headers, "{}")
            self.assertEqual(code, 502)
            self.assertEqual(json.loads(body)["result"], "SERVICE_UNCERTAIN")
            self.assertEqual(self.nginx_like_request(
                "POST", "/api/v1/mute", "correct-test-password", headers, "{}")[0], 403)
            self.assertEqual(fake.commands.count(b"MUTE\n"), 1)
        finally:
            fake.close()

    def test_production_incomplete_post_body_cannot_dispatch(self):
        self.stop_gateway()
        self.start_gateway(production_mute=True)
        fake = RecordingService(self.service_path)
        try:
            code, body, response_headers = self.nginx_like_request(
                "GET", "/api/v1/session", "correct-test-password")
            self.assertEqual(code, 200)
            request = (
                "POST /api/v1/mute HTTP/1.1\r\nHost: woodland-pi.local\r\n"
                "Origin: https://woodland-pi.local\r\nX-Forwarded-Proto: https\r\n"
                "X-Kenwood-Authenticated-User: remote\r\n"
                f"Cookie: {response_headers['Set-Cookie'].split(';', 1)[0]}\r\n"
                f"X-CSRF-Token: {json.loads(body)['csrf_token']}\r\n"
                "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n"
            ).encode()
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.connect(self.path)
                client.sendall(request)
                time.sleep(2.3)
            self.assertEqual(fake.commands, [b"STATUS\n"])
        finally:
            fake.close()

    def test_fake_nginx_request_and_unavailable_service(self):
        fake = FakeService(self.service_path, STATUS)
        code, body, _ = self.request("GET", "/api/v1/status",
                                     {"X-Kenwood-Authenticated-User": "remote"})
        fake.close()
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)["status"]["state"], "IDLE")
        self.assertEqual(fake.commands, [b"STATUS\n"])
        self.assertEqual(self.request("GET", "/api/v1/status")[0], 503)
        headers = {"X-Kenwood-Authenticated-User": "remote",
                   "Origin": "https://woodland-pi.local",
                   "X-Forwarded-Proto": "https", "Content-Type": "application/json"}
        with mock.patch.object(xs8_gateway, "send_mute") as send_mute:
            for path in ("/api/v1/mute", "/api/v1/source", "/api/v1/volume/step"):
                self.assertEqual(self.request("POST", path, headers, "{}")[0], 501)
            send_mute.assert_not_called()
        self.assertEqual(self.request("GET", "/api/v1/session", headers)[0], 404)

    def test_gateway_restart_invalidates_session(self):
        # Exercise session lifetime with a test-only handler gate and no service.
        self.stop_gateway()
        self.start_gateway(dev_mute=True)
        headers = {"X-Kenwood-Authenticated-User": "remote"}
        code, body, response_headers = self.request("GET", "/api/v1/session", headers)
        self.assertEqual(code, 200)
        old_cookie = response_headers["Set-Cookie"].split(";", 1)[0]
        old_token = json.loads(body)["csrf_token"]
        self.stop_gateway()
        self.start_gateway(dev_mute=True)
        code, body, response_headers = self.request(
            "GET", "/api/v1/session", {**headers, "Cookie": old_cookie})
        self.assertEqual(code, 200)
        self.assertNotEqual(response_headers["Set-Cookie"].split(";", 1)[0], old_cookie)
        self.assertNotEqual(json.loads(body)["csrf_token"], old_token)

    def test_peer_uid_check(self):
        request = mock.Mock()
        with mock.patch.object(xs8_gateway.socket, "SO_PEERCRED", 17, create=True):
            request.getsockopt.return_value = struct.pack("3i", 123, os.getuid(), 456)
            self.assertTrue(xs8_gateway.ActivatedUnixServer.verify_request(
                self.server, request, ""))
            request.getsockopt.return_value = struct.pack("3i", 123, os.getuid() + 1, 456)
            self.assertFalse(xs8_gateway.ActivatedUnixServer.verify_request(
                self.server, request, ""))
            request.getsockopt.side_effect = OSError("unsupported")
            self.assertFalse(xs8_gateway.ActivatedUnixServer.verify_request(
                self.server, request, ""))

    def test_systemd_mode_never_constructs_tcp_server(self):
        server = mock.Mock()
        with mock.patch.object(xs8_gateway, "systemd_listener") as inherited, \
             mock.patch.object(xs8_gateway, "ActivatedUnixServer", return_value=server), \
             mock.patch.object(xs8_gateway, "GatewayHTTPServer") as tcp:
            with contextlib.redirect_stdout(io.StringIO()):
                xs8_gateway.main(["--systemd-socket", "--nginx-uid", "33"])
        inherited.assert_called_once()
        tcp.assert_not_called()
        server.serve_forever.assert_called_once()
        server.server_close.assert_called_once()

    def test_offline_systemd_mode_keeps_tcp_closed_and_requires_fake_challenge(self):
        server = mock.Mock()
        with mock.patch.object(xs8_gateway, "systemd_listener") as inherited, \
             mock.patch.object(xs8_gateway, "ActivatedUnixServer", return_value=server) as unix, \
             mock.patch.object(xs8_gateway, "GatewayHTTPServer") as tcp:
            with contextlib.redirect_stdout(io.StringIO()):
                xs8_gateway.main(["--systemd-socket", "--nginx-uid", "33",
                                  "--enable-offline-mute", "--https-origin",
                                  "https://woodland-pi.local", "--socket",
                                  "/tmp/offline-fake-xs8.sock"])
        inherited.assert_called_once()
        tcp.assert_not_called()
        handler = unix.call_args.args[1]
        self.assertTrue(handler.dev_mute_enabled)
        self.assertEqual(handler.socket_path, "/tmp/offline-fake-xs8.sock")
        self.assertEqual(handler.https_origin, "https://woodland-pi.local")


if __name__ == "__main__":
    unittest.main()
