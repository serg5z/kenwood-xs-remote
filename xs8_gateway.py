#!/usr/bin/env python3
"""Local HTTP gateway; active controls require explicit socket-activated gates."""

import argparse
import ipaddress
import json
import os
import secrets
import socket
import socketserver
import struct
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlsplit


DEFAULT_SOCKET = "/tmp/kenwood-xs8.sock"
GATEWAY_SOCKET = "/run/kenwood-gateway.sock"
WEB_ROOT = Path(__file__).resolve().parent / "web"
STATUS_FIELDS = {
    "state", "stop_owed", "context_may_be_open", "monitor", "last_byte",
    "last_gpio_tx", "last_waveform", "last_stop_waveform",
    "receiver_execution",
}
SOURCES = ("CD", "TUNER", "PHONO", "TV1", "TV2", "TAPE1", "TAPE2")
EVIDENCED_SOURCES = ("CD", "TUNER", "PHONO", "TAPE1")


class ServiceError(Exception):
    pass


class MuteServiceError(Exception):
    def __init__(self, result, message, evidence=None):
        super().__init__(message)
        self.result = result
        self.evidence = evidence


def _send_control(socket_path, command, timeout=1.0, fake_only=True):
    """One socket request, one command, no retry after an uncertain outcome."""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(timeout)
            try:
                client.connect(socket_path)
            except socket.timeout as exc:
                raise MuteServiceError("TIMEOUT", "XS8 service connection timed out") from exc
            except OSError as exc:
                raise MuteServiceError("UNAVAILABLE", "XS8 service unavailable") from exc
            challenge_complete = not fake_only
            try:
                if fake_only:
                    # Keep the fake-only challenge and command on one connection.
                    challenge = secrets.token_hex(16).encode("ascii")
                    client.sendall(b"OFFLINE_TEST " + challenge + b"\n")
                    reply = bytearray()
                    while b"\n" not in reply:
                        chunk = client.recv(128)
                        if not chunk:
                            raise MuteServiceError("UNSAFE_SERVICE", "Fake service challenge failed")
                        reply.extend(chunk)
                        if len(reply) > 256:
                            raise MuteServiceError("UNSAFE_SERVICE", "Fake service challenge failed")
                    if bytes(reply) != b"OK OFFLINE_TEST " + challenge + b"\n":
                        raise MuteServiceError("UNSAFE_SERVICE", "Fake service challenge failed")
                    challenge_complete = True
                client.sendall(command)
                response = bytearray()
                while b"\n" not in response:
                    chunk = client.recv(128)
                    if not chunk:
                        raise MuteServiceError("DISCONNECTED", "XS8 service disconnected before reply")
                    response.extend(chunk)
                    if len(response) > 256:
                        raise MuteServiceError("INVALID_REPLY", "XS8 service reply too long")
            except socket.timeout as exc:
                if not challenge_complete:
                    raise MuteServiceError("UNSAFE_SERVICE", "Fake service challenge timed out") from exc
                raise MuteServiceError("TIMEOUT", "XS8 service reply timed out; delivery is uncertain") from exc
            except OSError as exc:
                if not challenge_complete:
                    raise MuteServiceError("UNSAFE_SERVICE", "Fake service challenge failed") from exc
                raise MuteServiceError("DISCONNECTED", "XS8 service disconnected; delivery is uncertain") from exc
    except socket.timeout as exc:
        raise MuteServiceError("TIMEOUT", "XS8 service connection timed out") from exc
    try:
        line = bytes(response).split(b"\n", 1)[0].decode("ascii")
    except UnicodeError as exc:
        raise MuteServiceError("INVALID_REPLY", "Invalid XS8 service reply") from exc
    return line


def send_mute(socket_path, timeout=1.0, fake_only=True):
    line = _send_control(socket_path, b"MUTE\n", timeout, fake_only)
    if line == "OK MUTE_TX":
        return line
    if line == "ERR MUTE_UNCERTAIN":
        raise MuteServiceError("SERVICE_UNCERTAIN", line)
    if line.startswith("ERR ") and len(line) > 4:
        raise MuteServiceError("REJECTED", line)
    raise MuteServiceError("INVALID_REPLY", "Invalid XS8 service reply")


def send_source(socket_path, source, timeout=1.0):
    if source not in EVIDENCED_SOURCES:
        raise MuteServiceError("REJECTED", "Source lacks controlled TX evidence")
    line = _send_control(socket_path, f"SELECT_SOURCE {source}\n".encode("ascii"),
                         timeout, fake_only=False)
    parts = line.split()
    if (len(parts) == 5 and parts[2] in ("gpio_tx=TX_OK", "gpio_tx=TX_FAIL") and
            parts[3] in ("waveform=MATCH", "waveform=MISMATCH", "waveform=INVALID",
                         "waveform=MISSING", "waveform=NOT_OBSERVED") and
            parts[4] == "receiver_execution=UNVERIFIED"):
        evidence = {"gpio_tx": parts[2].split("=", 1)[1],
                    "waveform": parts[3].split("=", 1)[1],
                    "receiver_execution": "UNVERIFIED"}
        if parts[:2] == ["OK", "SOURCE_TX"] and evidence == {
                "gpio_tx": "TX_OK", "waveform": "MATCH",
                "receiver_execution": "UNVERIFIED"}:
            return evidence
        if parts[:2] == ["ERR", "SOURCE_UNCERTAIN"]:
            raise MuteServiceError("SERVICE_UNCERTAIN", line, evidence)
    if line.startswith("ERR ") and len(line) > 4:
        raise MuteServiceError("REJECTED", line)
    raise MuteServiceError("INVALID_REPLY", "Invalid XS8 source reply")


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


def capabilities(mute_available=False, source_available=False):
    return {
        "api_version": "v0.1",
        "operations": {
            "status": {"classification": "supported", "enabled": True},
            "mute": {
                "classification": "unverified", "enabled": mute_available,
                "reason": "Receiver execution is unverified",
            },
            "volume_step": {
                "classification": "unsupported", "enabled": False,
                "reason": "Bounded atomic VOLUME_STEP is not implemented",
            },
            "source": {
                "classification": "unverified", "enabled": source_available,
                "available_sources": list(EVIDENCED_SOURCES),
                "blocked_sources": [s for s in SOURCES if s not in EVIDENCED_SOURCES],
                "reason": "TX and waveform do not confirm receiver execution; TV1, TV2 and TAPE2 lack controlled TX evidence",
            },
            "cd_transport": {
                "classification": "unsupported", "enabled": False,
                "reason": "CD transport is not implemented",
            },
        },
    }


class GatewayHandler(BaseHTTPRequestHandler):
    socket_path = DEFAULT_SOCKET
    dev_mute_enabled = False
    production_mute_enabled = False
    production_source_enabled = False
    https_origin = None
    sessions = None

    def mute_enabled(self):
        return self.dev_mute_enabled or self.production_mute_enabled

    def control_enabled(self):
        return self.mute_enabled() or self.production_source_enabled

    def authenticated_user(self):
        # nginx overwrites this header with $remote_user after Basic Auth.
        if not getattr(self.server, "proxy_peer_verified", False):
            return None
        users = self.headers.get_all("X-Kenwood-Authenticated-User", [])
        return users[0] if len(users) == 1 and users[0].strip() else None

    def address_string(self):
        return "local" if isinstance(self.client_address, str) else super().address_string()

    def current_session(self):
        user = self.authenticated_user()
        cookies = self.headers.get_all("Cookie", [])
        if not user or len(cookies) != 1:
            return None
        values = [part.strip().split("=", 1)[1] for part in cookies[0].split(";")
                  if part.strip().startswith("xs8_session=")]
        if len(values) != 1:
            return None
        session = self.sessions.get(values[0]) if self.sessions is not None else None
        if not session or session["user"] != user or session["expires"] < time.monotonic():
            return None
        return session

    def service_ready(self):
        try:
            status = read_status(self.socket_path)
        except ServiceError:
            return False
        return self.status_allows_mute(status)

    @staticmethod
    def status_allows_mute(status):
        return (status["state"] == "IDLE" and status["monitor"] == "HEALTHY"
                and status["stop_owed"] == "0" and status["context_may_be_open"] == "0")

    def send_body(self, code, body, content_type, headers=None):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'")
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, code, payload, headers=None):
        self.send_body(code, json.dumps(payload, separators=(",", ":")).encode(),
                       "application/json; charset=utf-8", headers)

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
            ready = self.current_session() is not None and self.service_ready()
            self.send_json(200, capabilities(self.mute_enabled() and ready,
                                              self.production_source_enabled and ready))
            return
        if path == "/api/v1/session" and self.control_enabled():
            user = self.authenticated_user()
            if not user:
                self.send_json(401, {"error": "Authentication required"})
                return
            session = self.current_session()
            if session is None:
                session_id = secrets.token_urlsafe(32)
                session = {"user": user, "csrf": secrets.token_urlsafe(32),
                           "expires": time.monotonic() + 1800}
                self.sessions[session_id] = session
                headers = {"Set-Cookie": f"xs8_session={session_id}; Path=/; Secure; HttpOnly; SameSite=Strict"}
            else:
                headers = None
            ready = self.service_ready()
            self.send_json(200, {"csrf_token": session["csrf"],
                                 "mute_available": self.mute_enabled() and ready,
                                 "source_available": self.production_source_enabled and ready}, headers)
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
        path = urlsplit(self.path).path
        is_mute = path == "/api/v1/mute" and self.mute_enabled()
        is_source = path == "/api/v1/source" and self.production_source_enabled
        if (not (is_mute or is_source) or
                not getattr(self.server, "proxy_peer_verified", False)):
            length = self.headers.get("Content-Length", "")
            if len(length) <= 4 and length.isdecimal() and 0 < int(length) <= 4096:
                self.connection.settimeout(2)
                try:
                    self.rfile.read(int(length))
                except (OSError, TimeoutError):
                    return
            self.send_json(501, {"error": "Control endpoint disabled"})
            return
        if self.headers.get_all("Origin", []) != [self.https_origin] or \
                self.headers.get_all("X-Forwarded-Proto", []) != ["https"]:
            self.send_json(403, {"error": "HTTPS Origin required"})
            return
        session = self.current_session()
        if session is None:
            self.send_json(401, {"error": "Authentication and session required"})
            return
        tokens = self.headers.get_all("X-CSRF-Token", [])
        if len(tokens) != 1 or not secrets.compare_digest(tokens[0], session["csrf"]):
            self.send_json(403, {"error": "Invalid CSRF token"})
            return
        lengths = self.headers.get_all("Content-Length", [])
        types = self.headers.get_all("Content-Type", [])
        if (len(lengths) != 1 or not lengths[0].isdecimal() or
                int(lengths[0]) > 64 or
                (is_mute and lengths[0] != "2") or
                types != ["application/json"] or self.headers.get("Transfer-Encoding")):
            self.send_json(400, {"error": "Invalid JSON request headers"})
            return
        self.connection.settimeout(2)
        try:
            body = self.rfile.read(int(lengths[0]))
        except (OSError, TimeoutError):
            return
        if is_mute:
            if body != b"{}":
                self.send_json(400, {"error": "Expected JSON body {}"})
                return
        else:
            try:
                pairs = json.loads(body, object_pairs_hook=list)
                source = (pairs[0][1] if isinstance(pairs, list) and
                          len(pairs) == 1 and isinstance(pairs[0], tuple) and
                          pairs[0][0] == "source" else None)
            except (UnicodeError, ValueError, TypeError, IndexError):
                source = None
            if source not in SOURCES:
                self.send_json(400, {"error": "Expected one allowlisted source"})
                return
            if source not in EVIDENCED_SOURCES:
                self.send_json(409, {"result": "SOURCE_UNVERIFIED",
                                     "receiver_execution": "UNVERIFIED"})
                return
            now = time.monotonic()
            if now - session.get("last_source_request", float("-inf")) < 1.0:
                session["csrf"] = secrets.token_urlsafe(32)
                self.send_json(429, {"result": "TOO_FAST",
                                     "receiver_execution": "UNVERIFIED"})
                return
            session["last_source_request"] = now
        # Single use: a repeated request with the same token cannot dispatch again.
        session["csrf"] = secrets.token_urlsafe(32)
        if (is_source or self.production_mute_enabled):
            try:
                status = read_status(self.socket_path)
            except ServiceError:
                self.send_json(503, {"result": "UNAVAILABLE", "receiver_execution": "UNVERIFIED"})
                return
            if not self.status_allows_mute(status):
                self.send_json(409, {"result": "NOT_ELIGIBLE", "receiver_execution": "UNVERIFIED"})
                return
        try:
            reply = (send_mute(self.socket_path, fake_only=not self.production_mute_enabled)
                     if is_mute else send_source(self.socket_path, source))
        except MuteServiceError as exc:
            code = {"UNAVAILABLE": 503, "REJECTED": 409, "TIMEOUT": 504}.get(exc.result, 502)
            payload = {"result": exc.result, "error": str(exc),
                       "receiver_execution": "UNVERIFIED"}
            if is_source and exc.evidence:
                payload.update(exc.evidence)
            self.send_json(code, payload)
            return
        payload = {"result": "SERVICE_TX_OK", "receiver_execution": "UNVERIFIED"}
        if is_mute:
            payload["service_reply"] = reply
        else:
            payload.update(reply)
        self.send_json(200, payload)


class ActivatedUnixServer(socketserver.TCPServer):
    """Serve HTTP on a pre-existing AF_UNIX listener; never bind TCP."""
    address_family = socket.AF_UNIX

    def __init__(self, listener, handler, allowed_uid):
        socketserver.BaseServer.__init__(self, listener.getsockname(), handler)
        self.socket = listener
        self.allowed_uid = allowed_uid
        self.proxy_peer_verified = True

    def verify_request(self, request, client_address):
        try:
            credentials = request.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                             struct.calcsize("3i"))
            _, uid, _ = struct.unpack("3i", credentials)
        except (AttributeError, OSError, struct.error):
            return False
        return uid == self.allowed_uid


def systemd_listener(expected_path=GATEWAY_SOCKET):
    if os.environ.get("LISTEN_PID") != str(os.getpid()) or os.environ.get("LISTEN_FDS") != "1":
        raise ValueError("expected exactly one systemd socket for this process")
    listener = socket.socket(fileno=3)
    if (listener.family != socket.AF_UNIX or listener.type != socket.SOCK_STREAM or
            listener.getsockname() != expected_path or
            not listener.getsockopt(socket.SOL_SOCKET, socket.SO_ACCEPTCONN)):
        listener.close()
        raise ValueError("systemd passed an unexpected listening socket")
    return listener


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int)
    parser.add_argument("--socket", default=DEFAULT_SOCKET,
                        help="local XS8 service Unix socket")
    parser.add_argument("--bind",
                        help="literal IPv4 address (default: 127.0.0.1)")
    parser.add_argument("--allow-insecure-readonly-lan", action="store_true",
                        help="explicitly allow unauthenticated read-only LAN HTTP")
    parser.add_argument("--enable-dev-mute", action="store_true",
                        help="reserved: legacy development gate remains disabled")
    parser.add_argument("--enable-offline-mute", action="store_true",
                        help="allow MUTE only after a fake service challenge")
    parser.add_argument("--enable-mute", action="store_true",
                        help="enable production MUTE through the real local XS8 Service")
    parser.add_argument("--enable-source", action="store_true",
                        help="enable evidenced source selection through the real local XS8 Service")
    parser.add_argument("--https-origin", help="required exact HTTPS Origin for active controls")
    parser.add_argument("--systemd-socket", action="store_true",
                        help="serve read-only HTTP using the systemd Unix listener")
    parser.add_argument("--nginx-uid", type=int,
                        help="expected nginx worker UID for --systemd-socket")
    args = parser.parse_args(argv)
    if args.enable_dev_mute:
        parser.error("MUTE development gate is disabled in phase 2A")
    if args.enable_offline_mute and (args.enable_mute or args.enable_source):
        parser.error("offline and production MUTE modes are mutually exclusive")
    if args.enable_offline_mute or args.enable_mute or args.enable_source:
        if not args.systemd_socket or not args.https_origin:
            parser.error("active controls require --systemd-socket and --https-origin")
        try:
            origin = urlsplit(args.https_origin)
            origin.port  # Reject malformed port syntax before accepting the origin.
        except ValueError:
            parser.error("--https-origin must be a valid HTTPS origin")
        if (origin.scheme != "https" or not origin.hostname or origin.username or
                origin.password or origin.netloc.endswith(":") or
                args.https_origin != f"https://{origin.netloc}"):
            parser.error("--https-origin must be an exact HTTPS origin without path or credentials")
        if args.enable_offline_mute:
            if (args.socket == DEFAULT_SOCKET or
                    Path(args.socket).resolve() == Path(DEFAULT_SOCKET).resolve()):
                parser.error("offline MUTE requires a non-default fake XS8 socket")
            try:
                if os.path.samefile(args.socket, DEFAULT_SOCKET):
                    parser.error("offline MUTE socket must not be the real XS8 Service socket")
            except FileNotFoundError:
                pass
        elif args.socket != DEFAULT_SOCKET:
            parser.error("production controls require the default XS8 Service socket")
    elif args.https_origin:
        parser.error("--https-origin requires an enabled control mode")
    if args.systemd_socket:
        if (args.nginx_uid is None or args.nginx_uid <= 0 or
                args.allow_insecure_readonly_lan or args.bind is not None or
                args.port is not None):
            parser.error("--systemd-socket requires --nginx-uid and no TCP or LAN options")
        return args
    if args.nginx_uid is not None:
        parser.error("--nginx-uid requires --systemd-socket")
    args.bind = args.bind or "127.0.0.1"
    args.port = 8080 if args.port is None else args.port
    if args.bind in ("0.0.0.0", "::"):
        parser.error("wildcard binding is not allowed; use the Pi's specific LAN IPv4 address")
    try:
        address = ipaddress.IPv4Address(args.bind)
    except ipaddress.AddressValueError:
        parser.error("--bind requires a literal IPv4 address")
    if address.is_loopback and args.allow_insecure_readonly_lan:
        parser.error("--allow-insecure-readonly-lan requires a non-loopback --bind address")
    if not address.is_loopback and not args.allow_insecure_readonly_lan:
        parser.error("non-loopback --bind requires --allow-insecure-readonly-lan")
    return args


def main(argv=None):
    args = parse_args(argv)
    if args.allow_insecure_readonly_lan:
        print("WARNING: unauthenticated, unencrypted read-only LAN preview; "
              "do not forward this port to the internet", file=sys.stderr)
    handler = type("ConfiguredGatewayHandler", (GatewayHandler,),
                   {"socket_path": args.socket, "dev_mute_enabled": args.enable_offline_mute,
                    "production_mute_enabled": args.enable_mute,
                    "production_source_enabled": args.enable_source,
                    "https_origin": args.https_origin, "sessions": {}})
    if args.systemd_socket:
        try:
            server = ActivatedUnixServer(systemd_listener(), handler, args.nginx_uid)
        except (OSError, ValueError) as exc:
            print(f"XS8 gateway socket activation failed: {exc}", file=sys.stderr)
            return 1
        mode = ("PRODUCTION CONTROLS" if args.enable_mute and args.enable_source else
                "PRODUCTION SOURCE" if args.enable_source else
                "PRODUCTION MUTE" if args.enable_mute else
                "OFFLINE FAKE MUTE TEST" if args.enable_offline_mute else "read-only")
        print(f"XS8 gateway listening on {GATEWAY_SOCKET} ({mode})")
    else:
        server = HTTPServer((args.bind, args.port), handler)
        print(f"XS8 gateway listening on http://{args.bind}:{server.server_port} (read-only)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
