import http.client
import contextlib
import io
import json
import socket
import tempfile
import threading
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
    def __init__(self, path, response):
        self.path = path
        self.response = response
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
            client.sendall(self.response)

    def close(self):
        self.thread.join(timeout=2)
        self.listener.close()
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

    def request(self, method, path):
        client = http.client.HTTPConnection("127.0.0.1", self.server.server_port,
                                            timeout=2)
        client.request(method, path)
        response = client.getresponse()
        result = response.status, response.read()
        client.close()
        return result

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


if __name__ == "__main__":
    unittest.main()
