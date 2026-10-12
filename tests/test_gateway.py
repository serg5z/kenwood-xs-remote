import http.client
import contextlib
import io
import json
import os
import socket
import struct
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
        self.server = xs8_gateway.HTTPServer(("127.0.0.1", 0), handler)
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
        self.assertEqual(len(parser.buttons), 13)
        self.assertTrue(all("disabled" in button for button in parser.buttons))
        for path in ("/api/v1/source", "/api/v1/mute",
                     "/api/v1/receiver/query",
                     "/api/v1/volume/step", "/api/v1/cd/transport",
                     "/api/v1/cd/track",
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
            # Simulate the browser's next action after the 1 s interval.
            for session in self.server.RequestHandlerClass.sessions.values():
                session["last_cd_track_request"] -= 1.0
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
            fake = FakeService(self.socket_path, None)
            code, body, _ = self.track(token, cookie)
            fake.close()
            self.assertEqual(code, 502)
            self.assertEqual(json.loads(body)["result"], "DISCONNECTED")
            self.assertTrue(json.loads(body)["physical_check_required"])
            self.assertEqual(fake.commands, [b"CD_TRACK_NEXT\n"])
        token, cookie = self.session()
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
        with mock.patch.object(xs8_gateway, "HTTPServer", return_value=server) as factory:
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
             mock.patch.object(xs8_gateway, "HTTPServer") as tcp:
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
             mock.patch.object(xs8_gateway, "HTTPServer") as tcp:
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
