#!/usr/bin/env python3
"""Local XS8 research console. All GPIO access remains in xs8-service."""

import argparse
import re
import shlex
import socket
import time


DEFAULT_SOCKET = "/tmp/kenwood-xs8.sock"
MAX_WINDOW_MS = 5000
BYTE_RE = re.compile(r"0x[0-9a-fA-F]{2}\Z")


def request(path, command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(5.0)
        connection.connect(path)
        connection.sendall((command + "\n").encode("ascii"))
        response = bytearray()
        while len(response) < 1024:
            part = connection.recv(1024 - len(response))
            if not part:
                raise ConnectionError("XS8 Service disconnected before reply")
            response.extend(part)
            if b"\n" in response:
                return response.split(b"\n", 1)[0].decode("ascii", "replace")
        raise ValueError("XS8 Service reply exceeded 1024 bytes")


def fields(reply):
    return dict(part.split("=", 1) for part in reply.split()
                if "=" in part)


def window_ms(value):
    if not value.isdecimal() or not 1 <= int(value) <= MAX_WINDOW_MS:
        raise ValueError("observation window must be 1..5000 ms")
    return int(value)


class Console:
    def __init__(self, socket_path=DEFAULT_SOCKET, output=print, pause=time.sleep):
        self.socket_path = socket_path
        self.output = output
        self.pause = pause
        self.default_window = 1500
        self.history = []

    def command(self, line):
        parts = shlex.split(line)
        if not parts:
            return True
        name = parts[0].lower()
        if name in ("quit", "exit") and len(parts) == 1:
            return False
        if name == "help" and len(parts) == 1:
            self.output("status | query | send 0xNN [milliseconds] | observe N | "
                        "set observe N | history | help | quit")
            return True
        if name == "history" and len(parts) == 1:
            for entry in self.history:
                self.output(entry)
            return True
        if name == "set" and len(parts) == 3 and parts[1].lower() == "observe":
            self.default_window = window_ms(parts[2])
            self.output(f"Default observation window: {self.default_window} ms")
            return True
        if name in ("status", "query") and len(parts) == 1:
            self.output(request(self.socket_path,
                                "STATUS" if name == "status" else "QUERY_STATUS"))
            return True
        if name == "send" and len(parts) in (2, 3):
            if not BYTE_RE.fullmatch(parts[1]):
                raise ValueError("send requires exactly one logical byte: 0xNN")
            window = window_ms(parts[2]) if len(parts) == 3 else self.default_window
            command = f"RESEARCH_SEND {parts[1]} {window}"
            self._experiment(command)
            return True
        if name == "observe" and len(parts) == 2:
            self._experiment(f"RESEARCH_OBSERVE {window_ms(parts[1])}")
            return True
        raise ValueError("unknown command or arguments; type help")

    def _experiment(self, command):
        reply = request(self.socket_path, command)
        self.output(reply)
        if not reply.startswith("OK RESEARCH_") and not reply.startswith(
                "ERR RESEARCH_TX "):
            self.history.append(f"{command}: {reply}")
            return
        session = fields(reply).get("id")
        if not session or not session.isdecimal():
            self.history.append(f"{command}: invalid session reply")
            return
        index = 0
        busy_until = time.monotonic() + 10.0
        try:
            while True:
                report = request(self.socket_path,
                                 f"RESEARCH_READ {session} {index}")
                if report.startswith("OK RESEARCH_RECORD "):
                    self.output(report)
                    index += 1
                elif report == "ERR BUSY" and time.monotonic() < busy_until:
                    self.pause(0.05)  # Another client owns the socket; never resend TX.
                elif report.startswith("OK RESEARCH_WAIT "):
                    self.pause(0.05)
                else:
                    self.output(report)
                    self.history.append(f"{command}: {reply}; {report}")
                    return
        except KeyboardInterrupt:
            try:
                request(self.socket_path, f"RESEARCH_CANCEL {session}")
            except OSError:
                pass
            self.output("Observation interrupted; XS8 transmission may already have occurred.")
            self.history.append(f"{command}: interrupted after {index} records")


def main():
    parser = argparse.ArgumentParser(description="Local XS8 research console")
    parser.add_argument("--socket", default=DEFAULT_SOCKET)
    args = parser.parse_args()
    console = Console(args.socket)
    print("XS8 research console. Type help. SEND requires the service's "
          "default-off --enable-research-tx authorization.")
    while True:
        try:
            if not console.command(input("xs8> ")):
                break
        except EOFError:
            break
        except KeyboardInterrupt:
            print("Interrupted locally; a submitted XS8 command may still "
                  "be running. No command was retried.")
        except (OSError, ValueError) as error:
            print(f"Error: {error}")


if __name__ == "__main__":
    main()
