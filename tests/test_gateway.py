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
        self.assertEqual(len(parser.buttons), 14)
        self.assertTrue(all("disabled" in button for button in parser.buttons))
        for path in ("/api/v1/source", "/api/v1/mute",
                     "/api/v1/volume/step", "/api/v1/cd/transport",
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
        fake = FakeService(self.socket_path, b"OK MUTE_TX\n")
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 200)
        self.assertEqual(fake.commands, [b"MUTE\n"])
        self.assertEqual(json.loads(body)["receiver_execution"], "UNVERIFIED")
        self.assertEqual(json.loads(body)["result"], "SERVICE_TX_OK")
        self.assertEqual(self.mute(token, cookie)[0], 403)

    def test_service_rejection_preserves_policy(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        fake = FakeService(self.socket_path, b"ERR MUTE_UNCERTAIN\n")
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 409)
        self.assertEqual(fake.commands, [b"MUTE\n"])
        self.assertEqual(json.loads(body)["result"], "REJECTED")

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
        fake = FakeService(self.socket_path, b"ERR MUTE_UNCERTAIN\n")
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 409)
        self.assertEqual(fake.commands, [b"MUTE\n"])

    def test_invalid_service_reply_is_not_success(self):
        self.enable_dev_mute()
        token, cookie = self.session()
        fake = FakeService(self.socket_path, b"OK OTHER\n")
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
        fake = FakeService(self.socket_path, None, delay=1.2)
        code, body, _ = self.mute(token, cookie)
        fake.close()
        self.assertEqual(code, 504)
        self.assertEqual(json.loads(body)["result"], "TIMEOUT")
        token, cookie = self.session()
        fake = FakeService(self.socket_path, None)
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
        fake = FakeService(self.socket_path, b"OK MUTE_TX\n")
        self.assertEqual(self.mute(token, cookie)[0], 200)
        fake.close()
        self.assertEqual(fake.commands, [b"MUTE\n"])

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

    def test_dev_gate_requires_https_loopback_and_nondefault_socket(self):
        for options in ([], ["--systemd-socket", "--nginx-uid", "1000"]):
            self.assertIn("disabled in phase 2A", self.parse_error(
                *options, "--enable-dev-mute", "--https-origin",
                "https://woodland-pi.local", "--socket", "/tmp/fake-xs8.sock"))

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


class UnixHTTPConnection(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__("localhost", timeout=2)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.path)


class ActivatedGatewayTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = str(Path(self.temp.name) / "gateway.sock")
        self.service_path = str(Path(self.temp.name) / "service.sock")
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(self.path)
        self.listener.listen(5)
        self.start_gateway()

    def start_gateway(self, dev_mute=False):
        handler = type("UnixTestHandler", (xs8_gateway.GatewayHandler,),
                       {"socket_path": self.service_path, "dev_mute_enabled": dev_mute,
                        "https_origin": "https://woodland-pi.local", "sessions": {},
                        "log_message": lambda *args: None})
        self.server = xs8_gateway.ActivatedUnixServer(
            self.listener.dup(), handler, os.getuid())
        # macOS lacks Linux SO_PEERCRED; credential checks are tested separately.
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
        client = UnixHTTPConnection(self.path)
        client.request(method, path, body=body, headers=headers or {})
        response = client.getresponse()
        result = response.status, response.read(), dict(response.getheaders())
        client.close()
        return result

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


if __name__ == "__main__":
    unittest.main()
