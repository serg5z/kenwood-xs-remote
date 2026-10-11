#!/usr/bin/env python3
"""Local, read-only HTTP gateway for the XS8 control service."""

import argparse
import json
import socket
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlsplit


DEFAULT_SOCKET = "/tmp/kenwood-xs8.sock"
WEB_ROOT = Path(__file__).resolve().parent / "web"
STATUS_FIELDS = {
    "state", "stop_owed", "context_may_be_open", "monitor", "last_byte",
    "last_gpio_tx", "last_waveform", "last_stop_waveform",
    "receiver_execution",
}


class ServiceError(Exception):
    pass


def read_status(socket_path, timeout=1.0):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(timeout)
            client.connect(socket_path)
            client.sendall(b"STATUS\n")
            response = bytearray()
            while b"\n" not in response:
                chunk = client.recv(512)
                if not chunk:
                    raise ServiceError("XS8 service closed before STATUS reply")
                response.extend(chunk)
                if len(response) > 4096:
                    raise ServiceError("XS8 service STATUS reply is too long")
    except (OSError, TimeoutError) as exc:
        raise ServiceError("XS8 service unavailable") from exc

    try:
        line = bytes(response).split(b"\n", 1)[0].decode("ascii")
        parts = line.split()
        if not parts or parts[0] != "OK":
            raise ValueError("STATUS was not accepted")
        fields = {}
        for part in parts[1:]:
            key, value = part.split("=", 1)
            if key not in STATUS_FIELDS or not value or key in fields:
                raise ValueError("invalid STATUS field")
            fields[key] = value
        if set(fields) != STATUS_FIELDS:
            raise ValueError("incomplete STATUS reply")
    except (UnicodeError, ValueError) as exc:
        raise ServiceError("Invalid XS8 service STATUS reply") from exc
    return fields


def capabilities():
    return {
        "api_version": "v0.1",
        "operations": {
            "status": {"classification": "supported", "enabled": True},
            "mute": {
                "classification": "unverified", "enabled": False,
                "reason": "XS8 service has MUTE, but HTTP control is not enabled; receiver execution is unverified",
            },
            "volume_step": {
                "classification": "unsupported", "enabled": False,
                "reason": "Bounded atomic VOLUME_STEP is not implemented",
            },
            "source": {
                "classification": "unsupported", "enabled": False,
                "reason": "Input selection is not implemented",
            },
            "cd_transport": {
                "classification": "unsupported", "enabled": False,
                "reason": "CD transport is not implemented",
            },
        },
    }


class GatewayHandler(BaseHTTPRequestHandler):
    socket_path = DEFAULT_SOCKET

    def send_body(self, code, body, content_type):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, code, payload):
        self.send_body(code, json.dumps(payload, separators=(",", ":")).encode(),
                       "application/json; charset=utf-8")

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/api/v1/status":
            try:
                status = read_status(self.socket_path)
            except ServiceError as exc:
                self.send_json(503, {"gateway": "online", "xs8_service": "unavailable",
                                     "error": str(exc)})
                return
            self.send_json(200, {"gateway": "online", "xs8_service": "connected",
                                 "status": status})
            return
        if path == "/api/v1/capabilities":
            self.send_json(200, capabilities())
            return
        assets = {
            "/": ("index.html", "text/html; charset=utf-8"),
            "/app.css": ("app.css", "text/css; charset=utf-8"),
            "/app.js": ("app.js", "text/javascript; charset=utf-8"),
        }
        if path in assets:
            name, content_type = assets[path]
            self.send_body(200, (WEB_ROOT / name).read_bytes(), content_type)
            return
        self.send_json(404, {"error": "Not found"})

    def do_POST(self):
        self.send_json(501, {"error": "Control endpoints are not implemented"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--socket", default=DEFAULT_SOCKET,
                        help="local XS8 service Unix socket")
    args = parser.parse_args()
    handler = type("ConfiguredGatewayHandler", (GatewayHandler,),
                   {"socket_path": args.socket})
    server = HTTPServer(("127.0.0.1", args.port), handler)
    print(f"XS8 gateway listening on http://127.0.0.1:{server.server_port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
