#!/usr/bin/env python3
"""Local XS8 research console. All GPIO access remains in xs8-service."""

import argparse
import importlib
import re
import shlex
import socket
import sys
import threading
import time


DEFAULT_SOCKET = "/tmp/kenwood-xs8.sock"
MAX_WINDOW_MS = 5000
MIN_GAP_MS = 50
MAX_GAP_MS = 1000
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


CONTEXT_PHRASE = "ENTER CONTEXT ATTENDED"
ROOT_CHECKLIST = (
    "ROOT verification (all three, independently; TX_OK/MATCH proves none):\n"
    "  1. query: two status frames that match the physically observed source/MONITOR\n"
    "  2. front panel: a non-motor source button visibly selects its input (not locked)\n"
    "  3. production SELECT_SOURCE from a different known input selects that input\n"
    "     (e.g. CD selects CD, not tuner preset CH8)\n"
    "On any failure: stop, keep the logs, recover physically (standby -> wake).")


class ContextSession:
    """Owner connection for one service CONTEXT session.

    The service exits CONTEXT (0x07) if this connection closes or the lease
    lapses, so a crashed or suspended console cannot leave it open.
    """

    def __init__(self, path, heartbeat_s, output):
        self.connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.connection.settimeout(5.0)
        self.connection.connect(path)
        self.lock = threading.Lock()
        self.buffer = bytearray()
        self.token = None
        self.lost = None
        self.output = output
        self.heartbeat_s = heartbeat_s
        self.stopped = threading.Event()
        self.thread = None

    def call(self, command):
        with self.lock:
            self.connection.sendall((command + "\n").encode("ascii"))
            while b"\n" not in self.buffer:
                if len(self.buffer) > 1024:
                    raise ValueError("XS8 Service reply exceeded 1024 bytes")
                part = self.connection.recv(1024)
                if not part:
                    raise ConnectionError("XS8 Service closed the CONTEXT owner connection")
                self.buffer.extend(part)
            line, _, rest = bytes(self.buffer).partition(b"\n")
            self.buffer = bytearray(rest)
            return line.decode("ascii", "replace")

    def start_heartbeat(self):
        def beat():
            while not self.stopped.wait(self.heartbeat_s):
                try:
                    reply = self.call(f"CONTEXT_SESSION_HEARTBEAT {self.token}")
                except (OSError, ValueError) as error:
                    reply = f"heartbeat failed: {error}"
                if reply != "OK CONTEXT_SESSION_LEASE_RENEWED":
                    self.lost = reply
                    self.output("CONTEXT session ended by the service: " + reply +
                                " -- check STATUS; the service attempts 0x07 itself.")
                    return
        self.thread = threading.Thread(target=beat, daemon=True)
        self.thread.start()

    def close(self):
        self.stopped.set()
        if self.thread and self.thread is not threading.current_thread():
            self.thread.join(timeout=2 * self.heartbeat_s + 1)
        self.connection.close()


# Refused inside CONTEXT before any transmission; the service enforces the same list.
CONTEXT_DENIED = {
    "0x17": "0x17 is refused inside CONTEXT (nested entry)",
    "0x07": "0x07 is the exit byte: use context exit",
    "0xe9": "0xE9 starts the volume motor; use the production volume path",
    "0xf9": "0xF9 starts the volume motor; use the production volume path",
}
SERVICE_DENIALS = {
    "NESTED_ENTRY_FORBIDDEN": CONTEXT_DENIED["0x17"],
    "USE_EXIT": CONTEXT_DENIED["0x07"],
    "VOLUME_MOTOR_FORBIDDEN": "0xE9/0xF9 start the volume motor; use the production volume path",
}


def parse_context_send(parts, default_window):
    """Exactly one byte inside CONTEXT; no chains, gaps or positional windows."""
    if len(parts) == 2:
        byte, window = parts[1], default_window
    elif len(parts) == 4 and parts[2] == "--observe-ms":
        byte, window = parts[1], window_ms(parts[3])
    else:
        raise ValueError("inside CONTEXT, send takes exactly one byte: "
                         "send 0xNN [--observe-ms N]; chains and --gap-ms are ROOT-only")
    if not BYTE_RE.fullmatch(byte):
        raise ValueError("inside CONTEXT, send takes exactly one logical byte: 0xNN")
    if byte.lower() in CONTEXT_DENIED:
        raise ValueError(CONTEXT_DENIED[byte.lower()] + "; nothing sent")
    return "0x" + byte[2:].upper(), window


def mask(text):
    return re.sub(r"(token=|CONTEXT_SESSION_\w+ )\d{4,}", r"\1<private>", text)


def fields(reply):
    return dict(part.split("=", 1) for part in reply.split()
                if "=" in part)


def window_ms(value):
    if not value.isdecimal() or not 1 <= int(value) <= MAX_WINDOW_MS:
        raise ValueError("observation window must be 1..5000 ms")
    return int(value)


def parse_send(parts, default_window):
    """Return one service request; never split a chain into socket requests."""
    bytes_ = []
    window = default_window
    gap = 200
    seen = set()
    index = 1
    while index < len(parts):
        part = parts[index]
        if part in ("--gap-ms", "--observe-ms"):
            if part in seen or index + 1 >= len(parts):
                raise ValueError(f"invalid {part} option")
            seen.add(part)
            value = parts[index + 1]
            if part == "--gap-ms":
                if not value.isdecimal() or not MIN_GAP_MS <= int(value) <= MAX_GAP_MS:
                    raise ValueError("gap must be 50..1000 ms")
                gap = int(value)
            else:
                window = window_ms(value)
            index += 2
        elif BYTE_RE.fullmatch(part) and len(bytes_) < 4 and not seen:
            bytes_.append(part)
            index += 1
        elif len(bytes_) == 1 and not seen and index == len(parts) - 1:
            window = window_ms(part)  # Legacy: send 0xNN [milliseconds].
            index += 1
        else:
            raise ValueError("send requires 1..4 logical bytes, then options")
    if not bytes_:
        raise ValueError("send requires 1..4 logical bytes: 0xNN")
    if len(bytes_) == 1:
        if "--gap-ms" in seen:
            raise ValueError("--gap-ms requires at least two bytes")
        return f"RESEARCH_SEND {bytes_[0]} {window}"
    return (f"RESEARCH_CHAIN {len(bytes_)} {gap} {window} " +
            " ".join(bytes_))


class Console:
    def __init__(self, socket_path=DEFAULT_SOCKET, output=print, pause=time.sleep,
                 confirm=input, context_heartbeat_s=1.0):
        self.socket_path = socket_path
        self.context_heartbeat_s = context_heartbeat_s
        self.session = None
        # Set when a session ended without a clean EXIT; ROOT send stays
        # blocked until context status confirms CONTEXT is closed.
        self.context_uncertain = False
        self.output = output
        self.pause = pause
        self.confirm = confirm
        self.context_token = None
        self.default_window = 1500
        self.history = []

    def _read_session(self, command, deadline):
        """Retry only indexed, read-only session requests after socket handoff."""
        while True:
            try:
                reply = request(self.socket_path, command)
                if reply != "ERR BUSY":
                    return reply
            except OSError as error:
                if time.monotonic() >= deadline:
                    raise ConnectionError(
                        f"observation read unavailable: {command}") from error
            if time.monotonic() >= deadline:
                raise ConnectionError(f"observation read busy: {command}")
            self.pause(0.05)

    def command(self, line):
        parts = shlex.split(line)
        if not parts:
            return True
        name = parts[0].lower()
        if name in ("quit", "exit") and len(parts) == 1:
            self.close_context()
            self.close_preparation()
            return False
        if name == "help" and len(parts) == 1:
            self.output("status | query | send 0xNN [milliseconds] | "
                        "send 0xNN 0xNN [up to 4] [--gap-ms N] "
                        "[--observe-ms N] | observe N | "
                        "context enter | (in CONTEXT) send 0xNN [--observe-ms N] | "
                        "context exit | context verify | context status | "
                        "context prepare MUTE | context observe [N] | "
                        "context run (blocked) | context cancel | "
                        "set observe N | history | help | quit")
            return True
        if name == "context":
            self._context(parts)
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
        if name == "send":
            if self.session or self.context_uncertain:
                # Never fall back to ROOT research TX while CONTEXT may be open.
                self._context_send(parts)
                return True
            command = parse_send(parts, self.default_window)
            self._experiment(command)
            return True
        if name == "observe" and len(parts) == 2:
            self._experiment(f"RESEARCH_OBSERVE {window_ms(parts[1])}")
            return True
        raise ValueError("unknown command or arguments; type help")

    def _context(self, parts):
        if parts[1:2] == ["send"]:
            raise ValueError("context send was removed; inside CONTEXT use: send 0xNN")
        if parts[1:2] in (["enter"], ["verify"]) or (
                parts == ["context", "exit"] and self.session):
            self._context_session(parts)
            return
        if parts == ["context", "status"] and (self.session or self.context_uncertain):
            reply = request(self.socket_path, "CONTEXT_SESSION_STATUS")
            self.output(reply)
            status = fields(reply)
            if (not self.session and status.get("open") == "0" and
                    status.get("context_may_be_open") == "0"):
                self.context_uncertain = False
                self.output("Service reports CONTEXT closed; ROOT send is available again. "
                            "Complete context verify before trusting ROOT behavior.")
        if parts == ["context", "status"]:
            reply = request(self.socket_path, "RESEARCH_CONTEXT_STATUS")
            self.output(reply)
            if fields(reply).get("state") != "PREPARED":
                self.context_token = None
            return
        if parts == ["context", "prepare", "MUTE"]:
            if self.context_token:
                raise ValueError("check context status or cancel the existing preparation")
            self.output("MUTE profile: proposed 0x17 → 0xD8 → 0x07. "
                        "Preparation reserves the service and records passive RX only. "
                        "RUN is blocked pending independent recovery evidence.")
            if self.confirm("Type PREPARE MUTE NO TX to confirm attended preparation: ") != \
                    "PREPARE MUTE NO TX":
                self.output("Preparation declined; no service command sent.")
                return
            reply = request(self.socket_path,
                            "RESEARCH_CONTEXT_PREPARE MUTE CONFIRM_NO_TX")
            self.output(re.sub(r"token=\d+", "token=<private>", reply))
            token = fields(reply).get("token")
            if reply.startswith("OK RESEARCH_CONTEXT_PREPARED ") and token and token.isdecimal():
                self.context_token = token
                self._experiment(
                    f"RESEARCH_CONTEXT_OBSERVE {token} {self.default_window}", token)
            return
        if parts in (["context", "cancel"], ["context", "exit"]):
            self.close_preparation()
            return
        if len(parts) in (2, 3) and parts[1] == "observe":
            if not self.context_token:
                raise ValueError("context prepare MUTE is required for passive preparation capture")
            window = self.default_window if len(parts) == 2 else window_ms(parts[2])
            self._experiment(
                f"RESEARCH_CONTEXT_OBSERVE {self.context_token} {window}",
                self.context_token)
            return
        if parts == ["context", "run"]:
            if not self.context_token:
                raise ValueError("context prepare MUTE is required; RUN remains blocked")
            reply = request(self.socket_path,
                            f"RESEARCH_CONTEXT_RUN {self.context_token}")
            self.output(reply)
            self.history.append("context run MUTE: " + reply)
            return
        raise ValueError("unknown context command; type help")

    def _context_session(self, parts):
        if self.context_uncertain and parts in (["context", "enter"], ["context", "verify"]):
            raise ValueError("CONTEXT state is uncertain after an abnormal end; nothing sent. "
                             "Run context status (and check the receiver) first")
        if parts == ["context", "enter"]:
            if self.session:
                raise ValueError("a CONTEXT session is already open; context exit first")
            if self.context_token:
                raise ValueError("cancel the MUTE preparation before entering CONTEXT")
            self.output("CONTEXT research: the service sends 0x17, then only the bytes you "
                        "type, one per command, never retried. Exit sends 0x07; a lost "
                        "connection, lapsed lease or shutdown makes the service send it. "
                        "0x07 is not proven to undo arbitrary commands: keep the front "
                        "panel and standby within reach. TX_OK/MATCH never proves execution.")
            if self.confirm(f"Type {CONTEXT_PHRASE} to send 0x17: ") != CONTEXT_PHRASE:
                self.output("CONTEXT entry declined; no service command sent.")
                return
            session = ContextSession(self.socket_path, self.context_heartbeat_s,
                                     self.output)
            try:
                reply = session.call(
                    f"CONTEXT_SESSION_ENTER CONFIRM_ATTENDED {self.default_window}")
            except BaseException:
                session.close()
                raise
            self.output(mask(reply))
            self.history.append("context enter: " + mask(reply))
            token = fields(reply).get("token")
            if reply.startswith("OK CONTEXT_SESSION_ENTERED ") and token and token.isdecimal():
                session.token = token
                self.session = session
                session.start_heartbeat()
            else:
                session.close()
                if "_UNCERTAIN" in reply:
                    self.context_uncertain = True
                    self.output("Entry uncertain: the service attempted 0x07 and latched "
                                "FAULT. Check the receiver physically.")
            self._capture(reply)
            return
        if parts == ["context", "verify"]:
            if self.session:
                raise ValueError("context exit first; verification runs in ROOT")
            self.output(request(self.socket_path, "QUERY_STATUS"))
            self.output(ROOT_CHECKLIST)
            return
        # context exit with an open session
        session = self.session
        try:
            reply = session.call(f"CONTEXT_SESSION_EXIT {session.token} {self.default_window}")
        except BaseException:
            self._drop_session(uncertain=True)
            raise
        self._drop_session(uncertain=not reply.startswith("OK CONTEXT_SESSION_EXITED "))
        self.output(reply)
        self.history.append("context exit: " + reply)
        self._capture(reply)
        if reply.startswith("OK CONTEXT_SESSION_EXITED "):
            self.output("0x07 matched; ROOT is not yet verified. Run: context verify")
        else:
            self.output("Exit uncertain: FAULT latched by the service. Recover physically.")
        self.output(ROOT_CHECKLIST)

    def _context_send(self, parts):
        if self.context_uncertain and not self.session:
            raise ValueError("CONTEXT state is uncertain after an abnormal end; nothing sent. "
                             "Run context status (and check the receiver) first")
        byte, window = parse_context_send(parts, self.default_window)
        session = self._live_session()
        try:
            reply = session.call(f"CONTEXT_SESSION_SEND {session.token} {byte} {window}")
        except (OSError, ValueError):
            self._drop_session(uncertain=True)
            raise
        self.output(reply)
        reason = fields(reply).get("reason")
        if reason in SERVICE_DENIALS:
            self.output(SERVICE_DENIALS[reason] + "; nothing sent")
        self.history.append(f"send {byte} (CONTEXT): {reply}")
        if reply.startswith(("ERR CONTEXT_SESSION_SEND_UNCERTAIN",
                             "ERR CONTEXT_SESSION reason=NOT_ACTIVE",
                             "ERR CONTEXT_SESSION reason=NOT_OWNER")):
            self._drop_session(uncertain=True)
            self.output("CONTEXT session ended. The byte is never retried. "
                        "Check context status and the receiver physically before continuing.")
        self._capture(reply)

    def _live_session(self):
        if not self.session:
            raise ValueError("no open CONTEXT session; context enter first")
        if self.session.lost:
            self._drop_session(uncertain=True)
            raise ValueError("the CONTEXT session already ended; nothing sent. "
                             "Run context status")
        return self.session

    def _drop_session(self, uncertain=False):
        session, self.session = self.session, None
        if uncertain:
            self.context_uncertain = True
        if session:
            session.close()

    def close_context(self):
        """Explicit EXIT if possible; closing the owner connection is the fallback."""
        if not self.session:
            return
        session = self.session
        clean = False
        try:
            if not session.lost:
                reply = session.call(
                    f"CONTEXT_SESSION_EXIT {session.token} {self.default_window}")
                self.output(reply)
                clean = reply.startswith("OK CONTEXT_SESSION_EXITED ")
        except (OSError, ValueError):
            self.output("Explicit EXIT unavailable; closing the owner connection makes "
                        "the service send 0x07.")
        finally:
            self._drop_session(uncertain=not clean)

    def _capture(self, reply):
        capture = fields(reply).get("capture_id")
        if not capture or not capture.isdecimal():
            return
        index = 0
        deadline = time.monotonic() + 10.0
        while True:
            report = self._read_session(f"RESEARCH_READ {capture} {index}", deadline)
            if report.startswith("OK RESEARCH_RECORD "):
                self.output(report)
                index += 1
            elif report.startswith("OK RESEARCH_WAIT "):
                self.pause(0.05)
            else:
                self.output(report)
                return

    def close_preparation(self):
        if not self.context_token:
            return
        token, self.context_token = self.context_token, None
        try:
            self.output(request(self.socket_path, f"RESEARCH_CONTEXT_CANCEL {token}"))
        except OSError:
            self.output("Preparation cancellation unavailable; its inactivity lease will expire. "
                        "No XS8 context was entered.")

    def _experiment(self, command, context_token=None):
        display_command = re.sub(r"(RESEARCH_CONTEXT_OBSERVE) \d+",
                                 r"\1 <token>", command)
        reply = request(self.socket_path, command)
        self.output(reply)
        if not reply.startswith("OK RESEARCH_") and not reply.startswith(
                "ERR RESEARCH_TX "):
            self.history.append(f"{display_command}: {reply}")
            return
        session = fields(reply).get("id")
        if not session or not session.isdecimal():
            self.history.append(f"{display_command}: invalid session reply")
            return
        index = 0
        tx_index = 0
        tx_done = not command.startswith("RESEARCH_CHAIN ")
        busy_until = time.monotonic() + 10.0
        heartbeat_at = time.monotonic() + 0.5
        try:
            while True:
                if context_token and time.monotonic() >= heartbeat_at:
                    renewed = request(self.socket_path,
                                      f"RESEARCH_CONTEXT_HEARTBEAT {context_token}")
                    if not renewed.startswith("OK RESEARCH_CONTEXT_LEASE_RENEWED "):
                        self.output(renewed)
                        self.history.append(f"{display_command}: preparation lease ended")
                        self.context_token = None
                        return
                    heartbeat_at = time.monotonic() + 0.5
                if not tx_done:
                    tx_report = self._read_session(
                        f"RESEARCH_TX_READ {session} {tx_index}", busy_until)
                    if tx_report.startswith("OK RESEARCH_TX_RECORD "):
                        self.output(tx_report)
                        tx_index += 1
                    elif tx_report.startswith("OK RESEARCH_TX_DONE "):
                        self.output(tx_report)
                        tx_done = True
                    elif tx_report.startswith("OK RESEARCH_TX_WAIT "):
                        self.pause(0.05)
                    else:
                        self.output(tx_report)
                        self.history.append(f"{display_command}: {reply}; {tx_report}")
                        return
                report = self._read_session(
                    f"RESEARCH_READ {session} {index}", busy_until)
                if report.startswith("OK RESEARCH_RECORD "):
                    self.output(report)
                    index += 1
                elif report.startswith("OK RESEARCH_WAIT "):
                    self.pause(0.05)
                elif report.startswith("OK RESEARCH_DONE ") and not tx_done:
                    self.pause(0.05)
                else:
                    self.output(report)
                    self.history.append(f"{display_command}: {reply}; {report}")
                    return
        except KeyboardInterrupt:
            if context_token:
                self.close_preparation()
                self.output("Passive preparation interrupted; no XS8 TX was attempted.")
                self.history.append(f"{display_command}: passive capture interrupted")
                return
            try:
                request(self.socket_path, f"RESEARCH_CANCEL {session}")
            except OSError:
                pass
            self.output("Observation interrupted; XS8 transmission may already have occurred.")
            self.history.append(f"{display_command}: interrupted after {index} records")


class LineHistory:
    """Session-local line editing and command history for an interactive terminal.

    readline's automatic history is switched off and only command lines are
    added here, so answers typed at confirmation prompts (for example the
    CONTEXT phrase) can never be recalled. Recalling a line only edits the
    prompt; Enter alone executes it. Nothing is read from or written to a file.
    """

    def __init__(self, module):
        self.readline = module
        self.last = None
        module.set_auto_history(False)
        clear = getattr(module, "clear_history", None)
        if clear:
            clear()

    def add(self, line):
        entry = line.strip()
        if not entry or entry == self.last:
            return
        self.readline.add_history(entry)
        self.last = entry


def enable_line_editing(stdin=None, stdout=None, importer=importlib.import_module):
    """LineHistory for a terminal session, or None (pipes, scripts, no readline)."""
    stdin = sys.stdin if stdin is None else stdin
    stdout = sys.stdout if stdout is None else stdout
    try:
        if not (stdin.isatty() and stdout.isatty()):
            return None
    except (AttributeError, ValueError):
        return None
    try:
        module = importer("readline")
    except ImportError:
        return None
    # Without this switch every confirmation answer would enter history.
    if not hasattr(module, "set_auto_history"):
        return None
    return LineHistory(module)


def main():
    parser = argparse.ArgumentParser(description="Local XS8 research console")
    parser.add_argument("--socket", default=DEFAULT_SOCKET)
    args = parser.parse_args()
    console = Console(args.socket)
    editor = enable_line_editing()
    print("XS8 research console. Type help. SEND requires the service's "
          "default-off --enable-research-tx authorization; chains also "
          "require --enable-research-chain; CONTEXT sessions also require "
          "--enable-research-context.")
    try:
        while True:
            try:
                line = input("xs8> ")
                if editor:
                    editor.add(line)
                if not console.command(line):
                    break
            except EOFError:
                break
            except KeyboardInterrupt:
                console.close_context()
                console.close_preparation()
                print("Interrupted locally; a submitted XS8 command may still "
                      "be running. No command was retried.")
            except (OSError, ValueError) as error:
                print(f"Error: {error}")
    finally:
        console.close_context()
        console.close_preparation()


if __name__ == "__main__":
    main()
