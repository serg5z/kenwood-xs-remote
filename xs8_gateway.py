#!/usr/bin/env python3
"""Local HTTP gateway; active controls require explicit socket-activated gates."""

import argparse
import ipaddress
import json
import os
import re
import secrets
import socket
import socketserver
import struct
import sys
import threading
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
RECEIVER_FIELDS = {
    "receiver_source", "receiver_source_ns", "receiver_source_evidence",
    "receiver_monitor", "receiver_monitor_ns", "receiver_monitor_evidence",
    "receiver_integrity",
}
CD_FIELDS = {"cd_state", "cd_action", "cd_release_owed",
             "cd_lease_expired", "cd_last_result"}
# Optional, from a service run with the experimental --enable-cd-presence-probe.
# Diagnostic only: never used for receiver_source, source selection or control
# eligibility. NOT_DETECTED is not OFF.
CD_PRESENCE_FIELDS = {"cd_changer", "cd_changer_ns", "cd_changer_last_probe"}
CD_PRESENCE_VALUES = ("PRESENT", "NOT_DETECTED", "UNKNOWN")
CD_PROBE_RESULTS = ("NONE", "RESPONSE", "NO_RESPONSE", "AMBIGUOUS", "FAILED")
# Optional RX-integrity diagnostics; operational health remains `monitor`.
CAPTURE_FIELDS = {"global_gap_policy", "global_gap_warnings", "capture_integrity",
                  "last_tx_global_gap_warnings", "last_tx_capture_integrity"}
OPTIONAL_STATUS_GROUPS = (RECEIVER_FIELDS, CD_FIELDS, CD_PRESENCE_FIELDS, CAPTURE_FIELDS)
SOURCES = ("CD", "TUNER", "PHONO", "VIDEO1", "VIDEO2", "TAPE1", "TAPE2")
# VIDEO1/VIDEO2 are atomic service CONTEXT sequences (0x17 -> 0x78/0x88 -> 0x07),
# physically validated 2026-10-12. TAPE2 (MONITOR) is not a source selection.
EVIDENCED_SOURCES = ("CD", "TUNER", "PHONO", "VIDEO1", "VIDEO2", "TAPE1")
CD_TRACK_ACTIONS = ("NEXT", "PREVIOUS")
CD_BLOCKED_ACTIONS = ("PLAY_PAUSE", "STOP", "SEEK_FORWARD", "SEEK_BACKWARD")
CD_WAVEFORMS = ("MATCH", "MISMATCH", "INVALID", "MISSING", "NOT_OBSERVED")
# Experimental spacing from one completed CD track dispatch to the next accepted
# one. The only physical evidence for separate presses is one ~100 ms
# RELEASE-to-PRESS chain; the bounds keep at least 2x that margin and allow
# returning to the Phase A 1 s value.
CD_TRACK_INTERVAL_MS = 250
CD_TRACK_INTERVAL_RANGE_MS = (200, 1000)
# Experimental: 0 removes the cooldown. Serialization, STATUS, CSRF and reply
# checks still apply, and each XS8 pair still completes before the next starts.
CD_TRACK_INTERVAL_OFF = 0
# Single ROOT key frames, mapped to bytes only by the service. STOP (ROOT 0xA1,
# formerly 0x9C) is physically observed; 0xBC PLAY/PAUSE is a
# historical mapping that is not yet physically verified. TIME_DISPLAY (0xEA),
# REPEAT (0xCC) and DISC_SKIP (0x8A) were physically verified by the operator on
# 2026-10-15. All are momentary: no display mode or REPEAT state is inferred.
CD_PLAYBACK_COMMANDS = {"PLAY_PAUSE": "CD_PLAY_PAUSE", "STOP": "CD_STOP",
                        "TIME_DISPLAY": "CD_TIME_DISPLAY", "REPEAT": "CD_REPEAT",
                        "DISC_SKIP": "CD_DISC_SKIP"}
CD_PLAYBACK_ACTIONS = tuple(CD_PLAYBACK_COMMANDS)
# Direct track entry. The service sends floor(N/10) ROOT 0xDA (+10) frames, then
# one digit frame, 200 ms apart; only the track number crosses this API. 1-39 is
# a conservative operational limit (at most three +10 frames), not a changer
# limit: the operator verified tracks 10, 13 and 23 (two +10 frames at most).
CD_TRACK_SELECT_RANGE = (1, 39)
CD_TRACK_SELECT_GAP_MS = 200
CD_PROVISIONAL_ACTIONS = ("PLAY_PAUSE",)
# Held FF/REW through the service's CD hold API. The service lease (2 s) and
# maximum (10 s) stay authoritative; these values only pace and bound clients.
CD_SEEK_DIRECTIONS = {"FORWARD": "SEEK_FORWARD", "BACKWARD": "SEEK_BACKWARD"}
CD_SEEK_HEARTBEAT_MS = 500
# The service's inactivity limit for a connection-owned KEY_DOWN hold.
CD_SEEK_LEASE_MS = 1200
CD_SEEK_MAX_HOLD_MS = 10000
# Gateway socket hygiene: close the owning service connection (which makes the
# service release at once) when the browser stops heartbeating for this long.
CD_SEEK_BROWSER_TIMEOUT_MS = 1500
VOLUME_DIRECTIONS = ("UP", "DOWN")
# Held VOL+/VOL- on the service's connection-owned volume hold. One absolute
# 20 s MAX_HOLD applies to CEC and Web holds alike: the service's --max-hold-ms
# (default 20000) is authoritative; this Gateway bound is an independent backstop.
VOLUME_HOLD_DIRECTIONS = {"UP": "VOLUME_UP", "DOWN": "VOLUME_DOWN"}
VOLUME_HOLD_MAX_MS = 20000
# A press shorter than this is a single VOLUME_STEP tap; longer starts a hold.
VOLUME_HOLD_DELAY_MS = 400
# Kinds of connection-owned holds. The service allows one hold of any kind, so
# the Gateway keeps a single registry for all of them.
KEY_HOLDS = {
    "cd": {"directions": CD_SEEK_DIRECTIONS, "max_ms": CD_SEEK_MAX_HOLD_MS,
           "label": "CD_SEEK"},
    "volume": {"directions": VOLUME_HOLD_DIRECTIONS, "max_ms": VOLUME_HOLD_MAX_MS,
               "label": "VOLUME_HOLD"},
}
# ROOT 0x52 POWER ON and ROOT 0x62 POWER OFF are both physically verified.
# Neither is assumed idempotent, and no power state is read back.
POWER_COMMANDS = {"ON": "POWER_ON", "OFF": "POWER_OFF"}
POWER_PROVISIONAL_ACTIONS = ()
POWER_INTERVAL_MS = 2000
# One-click POWER: the Gateway decides from fresh QUERY_STATUS SETTLE observations
# (never from TX evidence or a browser value) and sends at most one POWER frame.
POWER_TOGGLE_STATES = ("ON", "OFF_INFERRED")
POWER_SILENCE_GAP_S = 1.0
# CONTEXT 0x48: historical TAPE2 MONITOR toggle, not yet physically validated.
TAPE2_MONITOR_INTERVAL_MS = 1000
# Leaving a freshly observed CD source: SELECT_SOURCE first, then the same CD_STOP
# as the Web STOP button (ROOT 0xA1). Physically, STOP before SELECT_SOURCE made
# the changer start a track; VIDEO1 then STOP left VIDEO1 selected and the changer
# stopped. Selecting CD never stops it.
SOURCES_STOPPING_CD = ("TUNER", "PHONO", "VIDEO1", "VIDEO2", "TAPE1")
# Every server-wide control lock a source request holds, so no CD, volume, power
# or MONITOR request can run between its SELECT_SOURCE, its CD_STOP and its SYNC.
SOURCE_CONTROL_LOCKS = ("cd_track", "volume_step", "power", "tape2_monitor")
# A MONITOR toggle holds its own spacing lock and these until its SYNC answers.
MONITOR_CONTROL_LOCKS = ("cd_track", "volume_step", "power")
# Source selection and the MONITOR toggle end with one SYNC (QUERY_STATUS, ROOT
# 0x13) when --enable-receiver-query is on, only after a verified control.
# Server-wide spacing from one completed VOLUME_STEP dispatch to the next. Phase 1
# exposes single taps only; this guards against duplicate submissions, not hold.
VOLUME_STEP_INTERVAL_MS = 250


class ServiceError(Exception):
    pass


class MuteServiceError(Exception):
    def __init__(self, result, message, evidence=None):
        super().__init__(message)
        self.result = result
        self.evidence = evidence


class PowerToggleRefused(MuteServiceError):
    """A TOGGLE refused before any POWER frame was sent."""


class PowerObservation:
    """Gateway-owned, diagnostic receiver power observation. It changes only from
    QUERY_STATUS outcomes and is cleared by any POWER transmission; a TX result
    never sets it. A recognised reply is ON. Two clean (settled) NO_REPLY results
    1-10 s apart are OFF_INFERRED; a single one is never OFF. Other query faults
    leave the value to age."""

    FRESH_NS = 30_000_000_000
    SILENCE_GAP_NS = int(POWER_SILENCE_GAP_S * 1e9)
    SILENCE_SPAN_NS = 10_000_000_000

    def __init__(self):
        self.lock = threading.Lock()
        self.value, self.observed_ns, self.evidence = "UNKNOWN", 0, "NONE"
        self.silence_ns = None  # The first of a run of clean silences.

    def record(self, outcome, settled, now_ns=None):
        """outcome: a send_receiver_query result dict, or the MuteServiceError."""
        now_ns = now_ns or time.monotonic_ns()
        with self.lock:
            if isinstance(outcome, dict):
                if outcome.get("result") == "OBSERVATION_RECORDED":
                    self.value, self.observed_ns, self.evidence = "ON", now_ns, "QUERY"
                    self.silence_ns = None
                return
            silent = (outcome.result == "OBSERVATION_FAILED" and
                      (outcome.evidence or {}).get("reason") == "NO_REPLY")
            if not silent:
                self.silence_ns = None
                return
            if not settled:
                self.value, self.observed_ns, self.evidence = (
                    "UNKNOWN", now_ns, "UNSETTLED_SILENCE")
                self.silence_ns = None
                return
            first = self.silence_ns
            if first is not None and now_ns - first > self.SILENCE_SPAN_NS:
                first = None
            if first is not None and now_ns - first >= self.SILENCE_GAP_NS:
                self.value, self.observed_ns, self.evidence = (
                    "OFF_INFERRED", now_ns, "QUERY_SILENCE")
                self.silence_ns = None
                return
            if first is None:
                self.silence_ns = now_ns
            self.value, self.observed_ns, self.evidence = "UNKNOWN", now_ns, "SINGLE_SILENCE"

    def invalidate(self, now_ns=None):
        with self.lock:
            self.value, self.observed_ns, self.evidence = (
                "UNKNOWN", now_ns or time.monotonic_ns(), "POWER_TX")
            self.silence_ns = None

    def snapshot(self, now_ns=None):
        now_ns = now_ns or time.monotonic_ns()
        with self.lock:
            age_ms = (max(0, now_ns - self.observed_ns) // 1_000_000
                      if self.observed_ns else None)
            freshness = ("UNKNOWN" if self.value == "UNKNOWN" else
                         "FRESH" if now_ns - self.observed_ns <= self.FRESH_NS else "STALE")
            return {"value": self.value, "observed_mono_ns": self.observed_ns,
                    "age_ms": age_ms, "evidence": self.evidence, "freshness": freshness,
                    "diagnostic": True, "receiver_execution": "UNVERIFIED"}


def _send_control(socket_path, command, timeout=1.0, fake_only=True,
                  max_reply=256):
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
                    if len(response) > max_reply:
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


def send_source(socket_path, source, timeout=3.0):
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


def send_cd_track(socket_path, action, timeout=3.0):
    """One discrete PRESS/RELEASE pair; the service owns RELEASE and fault latching."""
    if action not in CD_TRACK_ACTIONS:
        raise MuteServiceError("REJECTED", "CD action is not allowlisted")
    line = _send_control(socket_path, f"CD_TRACK_{action}\n".encode("ascii"),
                         timeout, fake_only=False, max_reply=512)
    parts = line.split()
    if len(parts) == 9 and parts[2] == f"action={action}" and \
            parts[8] == "receiver_execution=UNVERIFIED":
        keys = ("press_gpio_tx", "press_waveform", "release_gpio_tx",
                "release_waveform", "cd_result")
        evidence = {}
        for key, part in zip(keys, parts[3:8]):
            name, _, value = part.partition("=")
            if name != key:
                break
            evidence[key] = value
        valid = (len(evidence) == 5 and
                 evidence["press_gpio_tx"] in ("TX_OK", "TX_FAIL") and
                 evidence["release_gpio_tx"] in ("TX_OK", "TX_FAIL") and
                 evidence["press_waveform"] in CD_WAVEFORMS and
                 evidence["release_waveform"] in CD_WAVEFORMS)
        if valid:
            evidence["receiver_execution"] = "UNVERIFIED"
            if parts[:2] == ["OK", "CD_TRACK_TX"] and evidence == {
                    "press_gpio_tx": "TX_OK", "press_waveform": "MATCH",
                    "release_gpio_tx": "TX_OK", "release_waveform": "MATCH",
                    "cd_result": "TX_VERIFIED", "receiver_execution": "UNVERIFIED"}:
                return evidence
            if parts[:2] == ["ERR", "CD_TRACK_UNCERTAIN"] and evidence["cd_result"] in (
                    "PRESS_UNCERTAIN", "RELEASE_UNCERTAIN"):
                raise MuteServiceError("SERVICE_UNCERTAIN", line, evidence)
    if line == "ERR CD_FAULT_LATCHED":
        raise MuteServiceError("CD_FAULT_LATCHED", line)
    # A malformed TX report is uncertain, never a clean pre-TX refusal.
    if (line.startswith("ERR ") and len(parts) >= 2 and
            parts[1] not in ("CD_TRACK_TX", "CD_TRACK_UNCERTAIN")):
        raise MuteServiceError("REJECTED", line)
    raise MuteServiceError("INVALID_REPLY", "Invalid XS8 CD track reply")


def cd_track_select_frames(track):
    return track // 10 + 1


def send_cd_track_select(socket_path, track, timeout=None):
    """One direct track entry as a single service transaction; never retried."""
    if (not isinstance(track, int) or isinstance(track, bool) or
            not CD_TRACK_SELECT_RANGE[0] <= track <= CD_TRACK_SELECT_RANGE[1]):
        raise MuteServiceError("REJECTED", "CD track is outside the allowed range")
    frames = cd_track_select_frames(track)
    if timeout is None:
        # Each frame is about 0.1 s and each gap 0.2 s; leave a wide margin
        # (5 s for the four frames of tracks 30-39).
        timeout = 3.0 + 0.5 * frames
    line = _send_control(socket_path, f"CD_TRACK_SELECT {track}\n".encode("ascii"),
                         timeout, fake_only=False, max_reply=512)
    parts = line.split()
    if len(parts) == 9 and parts[8] == "receiver_execution=UNVERIFIED":
        evidence = _evidence(parts[2:8], ("track", "frames", "sent", "gpio_tx",
                                          "waveform", "cd_result"))
        if (evidence and evidence["track"] == str(track) and
                evidence["frames"] == str(frames) and evidence["sent"].isdecimal() and
                1 <= int(evidence["sent"]) <= frames and
                evidence["gpio_tx"] in ("TX_OK", "TX_FAIL") and
                evidence["waveform"] in CD_WAVEFORMS):
            del evidence["track"]
            evidence["receiver_execution"] = "UNVERIFIED"
            if parts[:2] == ["OK", "CD_TRACK_SELECT_TX"] and evidence == {
                    "frames": str(frames), "sent": str(frames), "gpio_tx": "TX_OK",
                    "waveform": "MATCH", "cd_result": "TX_VERIFIED",
                    "receiver_execution": "UNVERIFIED"}:
                return evidence
            if (parts[:2] == ["ERR", "CD_TRACK_SELECT_UNCERTAIN"] and
                    evidence["cd_result"] in ("TX_UNCERTAIN", "CANCELLED")):
                raise MuteServiceError("SERVICE_UNCERTAIN", line, evidence)
    if line == "ERR CD_FAULT_LATCHED":
        raise MuteServiceError("CD_FAULT_LATCHED", line)
    # A malformed TX report is uncertain, never a clean pre-TX refusal.
    if (line.startswith("ERR ") and len(parts) >= 2 and
            parts[1] not in ("CD_TRACK_SELECT_TX", "CD_TRACK_SELECT_UNCERTAIN")):
        raise MuteServiceError("REJECTED", line)
    raise MuteServiceError("INVALID_REPLY", "Invalid XS8 CD track selection reply")


def _evidence(parts, keys):
    evidence = {}
    for key, part in zip(keys, parts):
        name, _, value = part.partition("=")
        if name != key:
            return None
        evidence[key] = value
    return evidence if len(evidence) == len(keys) else None


def send_cd_playback(socket_path, action, timeout=3.0):
    """One single-frame CD key; no KEY UP exists and the service never retries."""
    if action not in CD_PLAYBACK_COMMANDS:
        raise MuteServiceError("REJECTED", "CD action is not allowlisted")
    line = _send_control(socket_path, f"{CD_PLAYBACK_COMMANDS[action]}\n".encode("ascii"),
                         timeout, fake_only=False, max_reply=512)
    parts = line.split()
    if len(parts) == 6 and parts[2] == f"action={action}" and \
            parts[5] == "receiver_execution=UNVERIFIED":
        evidence = _evidence(parts[3:5], ("gpio_tx", "waveform"))
        if (evidence and evidence["gpio_tx"] in ("TX_OK", "TX_FAIL") and
                evidence["waveform"] in CD_WAVEFORMS):
            evidence["receiver_execution"] = "UNVERIFIED"
            if parts[:2] == ["OK", "CD_TRANSPORT_TX"] and \
                    evidence["gpio_tx"] == "TX_OK" and evidence["waveform"] == "MATCH":
                return evidence
            if parts[:2] == ["ERR", "CD_TRANSPORT_UNCERTAIN"]:
                raise MuteServiceError("SERVICE_UNCERTAIN", line, evidence)
    if line == "ERR CD_FAULT_LATCHED":
        raise MuteServiceError("CD_FAULT_LATCHED", line)
    if (line.startswith("ERR ") and len(parts) >= 2 and
            not parts[1].startswith("CD_TRANSPORT")):
        raise MuteServiceError("REJECTED", line)
    raise MuteServiceError("INVALID_REPLY", "Invalid XS8 CD playback reply")


class ServiceConnection:
    """One persistent XS8 Service socket that owns at most one KEY_DOWN hold.

    The service releases the hold as soon as this socket closes, so close() is
    the emergency release. Any error or unexpected reply closes it; nothing here
    reconnects or resends."""

    def __init__(self, socket_path, timeout):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self.sock.settimeout(timeout)
            self.sock.connect(socket_path)
        except socket.timeout as exc:
            self.close()
            raise MuteServiceError("TIMEOUT", "XS8 service connection timed out") from exc
        except OSError as exc:
            self.close()
            raise MuteServiceError("UNAVAILABLE", "XS8 service unavailable") from exc

    @property
    def closed(self):
        return self.sock is None

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def exchange(self, line, timeout, max_reply=512):
        """Send one line and read exactly one reply line; close on any failure."""
        if self.sock is None:
            raise MuteServiceError("DISCONNECTED", "XS8 service connection is closed")
        try:
            self.sock.settimeout(timeout)
            self.sock.sendall(line.encode("ascii") + b"\n")
            response = bytearray()
            while b"\n" not in response:
                chunk = self.sock.recv(128)
                if not chunk:
                    raise MuteServiceError("DISCONNECTED",
                                           "XS8 service disconnected; delivery is uncertain")
                response.extend(chunk)
                if len(response) > max_reply:
                    raise MuteServiceError("INVALID_REPLY", "XS8 service reply too long")
            if not response.endswith(b"\n") or response.count(b"\n") != 1:
                raise MuteServiceError("INVALID_REPLY", "Unexpected extra XS8 service data")
            return bytes(response[:-1]).decode("ascii")
        except socket.timeout as exc:
            self.close()
            raise MuteServiceError("TIMEOUT", "XS8 service reply timed out; delivery is uncertain") from exc
        except UnicodeError as exc:
            self.close()
            raise MuteServiceError("INVALID_REPLY", "Invalid XS8 service reply") from exc
        except OSError as exc:
            self.close()
            raise MuteServiceError("DISCONNECTED", "XS8 service disconnected; delivery is uncertain") from exc
        except MuteServiceError:
            self.close()
            raise


def _single_frame_reply(line, ok_head, uncertain_head, prefix_fields=()):
    """Strict OK/uncertain reply carrying gpio_tx and waveform; refusals raise."""
    parts = line.split()
    count = len(ok_head) + len(prefix_fields) + 3
    if len(parts) == count and parts[-1] == "receiver_execution=UNVERIFIED" and \
            parts[len(ok_head):len(ok_head) + len(prefix_fields)] == list(prefix_fields):
        evidence = _evidence(parts[-3:-1], ("gpio_tx", "waveform"))
        if (evidence and evidence["gpio_tx"] in ("TX_OK", "TX_FAIL") and
                evidence["waveform"] in CD_WAVEFORMS):
            evidence["receiver_execution"] = "UNVERIFIED"
            if parts[:len(ok_head)] == list(ok_head) and \
                    evidence["gpio_tx"] == "TX_OK" and evidence["waveform"] == "MATCH":
                return evidence
            if parts[:len(uncertain_head)] == list(uncertain_head):
                raise MuteServiceError("SERVICE_UNCERTAIN", line, evidence)
    if line == "ERR CD_FAULT_LATCHED":
        raise MuteServiceError("CD_FAULT_LATCHED", line)
    if line == "ERR FAULT_LATCHED":
        raise MuteServiceError("FAULT_LATCHED", line)
    if (line.startswith("ERR ") and len(parts) >= 2 and
            parts[1] not in (ok_head[1], uncertain_head[1])):
        raise MuteServiceError("REJECTED", line)
    raise MuteServiceError("INVALID_REPLY", "Invalid XS8 service reply")


def send_power(socket_path, action, timeout=2.0):
    """One ROOT POWER frame; the service refuses it during any hold or obligation."""
    if action not in POWER_COMMANDS:
        raise MuteServiceError("REJECTED", "Power action is not allowlisted")
    line = _send_control(socket_path, f"{POWER_COMMANDS[action]}\n".encode("ascii"),
                         timeout, fake_only=False)
    return _single_frame_reply(line, ("OK", "POWER_TX"), ("ERR", "POWER_UNCERTAIN"),
                               (f"action={action}",))


def send_tape2_monitor(socket_path, timeout=3.0):
    """One atomic CONTEXT 0x17 -> 0x48 -> 0x07 toggle; never changes the indicator."""
    line = _send_control(socket_path, b"TAPE2_MONITOR_TOGGLE\n", timeout, fake_only=False)
    return _single_frame_reply(line, ("OK", "TAPE2_MONITOR_TX"),
                               ("ERR", "TAPE2_MONITOR_UNCERTAIN"))


def _volume_uncertain(parts, line):
    """The volume form of an uncertain KEY reply: service state fields."""
    evidence = _evidence(parts[3:6], ("state", "stop_owed", "context_may_be_open"))
    if (evidence and evidence["state"] and evidence["stop_owed"] in ("0", "1") and
            evidence["context_may_be_open"] in ("0", "1")):
        evidence["receiver_execution"] = "UNVERIFIED"
        raise MuteServiceError("SERVICE_UNCERTAIN", line, evidence)


def parse_key_down(line, action):
    """Strict KEY_DOWN start reply. Raises for anything but a verified press."""
    parts = line.split()
    if parts == ["OK", "KEY_DOWN_TX", f"action={action}", "gpio_tx=TX_OK", "waveform=MATCH",
                 "receiver_execution=UNVERIFIED"]:
        return {"gpio_tx": "TX_OK", "waveform": "MATCH", "receiver_execution": "UNVERIFIED"}
    if (len(parts) == 7 and parts[:2] == ["ERR", "KEY_DOWN_UNCERTAIN"] and
            parts[2] == f"action={action}" and parts[6] == "receiver_execution=UNVERIFIED"):
        _volume_uncertain(parts, line)
    if (len(parts) == 8 and parts[:2] == ["ERR", "KEY_DOWN_UNCERTAIN"] and
            parts[2] == f"action={action}" and parts[7] == "receiver_execution=UNVERIFIED"):
        evidence = _evidence(parts[3:7], ("gpio_tx", "waveform", "release_gpio_tx",
                                          "release_waveform"))
        if evidence:
            evidence["receiver_execution"] = "UNVERIFIED"
            raise MuteServiceError("SERVICE_UNCERTAIN", line, evidence)
    if line == "ERR CD_FAULT_LATCHED":
        raise MuteServiceError("CD_FAULT_LATCHED", line)
    if (line.startswith("ERR ") and len(parts) >= 2 and
            not parts[1].startswith("KEY_DOWN")):
        raise MuteServiceError("REJECTED", line)
    raise MuteServiceError("INVALID_REPLY", "Invalid XS8 KEY_DOWN reply")


def parse_key_up(line, action):
    """Strict KEY_UP reply: released, already ended, or uncertain."""
    parts = line.split()
    if (len(parts) == 6 and parts[2] == f"action={action}" and
            parts[5] == "receiver_execution=UNVERIFIED"):
        evidence = _evidence(parts[3:5], ("gpio_tx", "waveform"))
        if evidence and evidence["gpio_tx"] in ("TX_OK", "TX_FAIL") and \
                evidence["waveform"] in CD_WAVEFORMS:
            evidence["receiver_execution"] = "UNVERIFIED"
            if parts[:2] == ["OK", "KEY_UP_TX"] and \
                    evidence["gpio_tx"] == "TX_OK" and evidence["waveform"] == "MATCH":
                return "RELEASED", evidence
            if parts[:2] == ["ERR", "KEY_UP_UNCERTAIN"]:
                raise MuteServiceError("SERVICE_UNCERTAIN", line, evidence)
    if (len(parts) == 7 and parts[:2] == ["ERR", "KEY_UP_UNCERTAIN"] and
            parts[2] == f"action={action}" and parts[6] == "receiver_execution=UNVERIFIED"):
        _volume_uncertain(parts, line)
    if len(parts) == 3 and parts[:2] == ["OK", "KEY_UP_NONE"] and \
            parts[2].startswith("last=") and len(parts[2]) > 5:
        return "NOT_ACTIVE", {"cd_last_result": parts[2][5:]}
    raise MuteServiceError("INVALID_REPLY", "Invalid XS8 KEY_UP reply")


def send_volume_step(socket_path, direction, timeout=3.0):
    """One atomic VOLUME_STEP; the service owns STOP, ROOT exit and fault latching."""
    if direction not in VOLUME_DIRECTIONS:
        raise MuteServiceError("REJECTED", "Volume direction is not allowlisted")
    line = _send_control(socket_path, f"VOLUME_STEP {direction}\n".encode("ascii"),
                         timeout, fake_only=False, max_reply=512)
    parts = line.split()
    if len(parts) == 7 and parts[2] == f"direction={direction}" and \
            parts[6] == "receiver_execution=UNVERIFIED":
        keys = ("state", "stop_owed", "context_may_be_open")
        evidence = {}
        for key, part in zip(keys, parts[3:6]):
            name, _, value = part.partition("=")
            if name != key:
                break
            evidence[key] = value
        valid = (len(evidence) == 3 and evidence["state"] and
                 evidence["stop_owed"] in ("0", "1") and
                 evidence["context_may_be_open"] in ("0", "1"))
        if valid:
            evidence["receiver_execution"] = "UNVERIFIED"
            if parts[:2] == ["OK", "VOLUME_STEP_TX"] and evidence == {
                    "state": "IDLE", "stop_owed": "0", "context_may_be_open": "0",
                    "receiver_execution": "UNVERIFIED"}:
                evidence["volume_result"] = "TX_VERIFIED"
                return evidence
            if parts[:2] in (["ERR", "VOLUME_STEP_UNCERTAIN"],
                             ["ERR", "VOLUME_STEP_CANCELLED"]):
                evidence["volume_result"] = ("CANCELLED" if parts[1].endswith("CANCELLED")
                                             else "UNCERTAIN")
                raise MuteServiceError("SERVICE_UNCERTAIN", line, evidence)
    if line == "ERR CD_FAULT_LATCHED":
        raise MuteServiceError("CD_FAULT_LATCHED", line)
    if line == "ERR FAULT_LATCHED":
        raise MuteServiceError("FAULT_LATCHED", line)
    # A malformed TX report is uncertain, never a clean pre-TX refusal.
    if (line.startswith("ERR ") and len(parts) >= 2 and
            not parts[1].startswith("VOLUME_STEP")):
        raise MuteServiceError("REJECTED", line)
    raise MuteServiceError("INVALID_REPLY", "Invalid XS8 volume step reply")


QUERY_REASON = re.compile(r"[A-Z][A-Z0-9_]{0,31}")
MONITOR_VALUES = ("ON", "OFF")


def send_receiver_query(socket_path, settle=False):
    """One ROOT 0x13 QUERY_STATUS, never retried. With settle, the service first
    waits (at most 1.5 s, sending nothing) for a quiet bus. Returns the recorded
    observation; raises with the service's specific reason otherwise."""
    command = b"QUERY_STATUS SETTLE\n" if settle else b"QUERY_STATUS\n"
    line = _send_control(socket_path, command, timeout=3.0,
                         fake_only=False, max_reply=2048)
    parts = line.split()
    if (line.startswith("OK QUERY_STATUS reason=NONE ") and
            " gpio_tx=TX_OK waveform=MATCH " in line and
            " tx=" in line and " rx1=" in line and " rx2=" in line and
            parts[-1] == "receiver_execution=UNVERIFIED"):
        inferred = dict(part.split("=", 1) for part in parts
                        if part.startswith(("source_inferred=", "monitor_inferred=")))
        source = inferred.get("source_inferred")
        monitor = inferred.get("monitor_inferred")
        if source in SOURCES and monitor in MONITOR_VALUES:
            return {"result": "OBSERVATION_RECORDED", "observed_source": source,
                    "observed_monitor": monitor}
        if source in SOURCES + ("UNKNOWN",) and monitor in MONITOR_VALUES + ("UNKNOWN",):
            # Two frames arrived, but not a recognised source/MONITOR pair.
            raise MuteServiceError("UNRECOGNIZED", line, {"reason": "UNRECOGNIZED"})
        raise MuteServiceError("INVALID_REPLY", "Invalid XS8 query reply")
    if parts[:2] == ["ERR", "QUERY_STATUS"]:
        reason = (parts[2][len("reason="):]
                  if len(parts) > 2 and parts[2].startswith("reason=") else "")
        if not QUERY_REASON.fullmatch(reason):
            raise MuteServiceError("INVALID_REPLY", "Invalid XS8 query reply")
        result = "SERVICE_UNCERTAIN" if reason == "TX_UNCERTAIN" else "OBSERVATION_FAILED"
        raise MuteServiceError(result, line, {"reason": reason})
    if line.startswith("ERR "):
        reason = parts[1] if len(parts) > 1 and QUERY_REASON.fullmatch(parts[1]) else None
        raise MuteServiceError("REJECTED", line, {"reason": reason} if reason else None)
    raise MuteServiceError("INVALID_REPLY", "Invalid XS8 query reply")


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
            if (key not in STATUS_FIELDS.union(*OPTIONAL_STATUS_GROUPS) or
                    not value or key in fields):
                raise ValueError("invalid STATUS field")
            fields[key] = value
        # Each optional group is reported whole or not at all.
        if (not STATUS_FIELDS <= set(fields) or
                any(group & set(fields) not in (set(), group)
                    for group in OPTIONAL_STATUS_GROUPS)):
            raise ValueError("incomplete STATUS reply")
        if CAPTURE_FIELDS <= set(fields):
            if (fields["global_gap_policy"] not in ("strict", "warn") or
                    fields["capture_integrity"] not in
                    ("FAILED", "WARN_GLOBAL_GAP", "NO_KNOWN_GAPS") or
                    fields["last_tx_capture_integrity"] not in
                    ("NOT_OBSERVED", "FAILED", "WARN_GLOBAL_GAP", "NO_KNOWN_GAPS", "NOT_ESTABLISHED")):
                raise ValueError("invalid capture diagnostics")
            for key in ("global_gap_warnings", "last_tx_global_gap_warnings"):
                value = fields[key]
                if (not value.isdecimal() or value != str(int(value)) or int(value) >= 1 << 64):
                    raise ValueError("invalid capture counter")
        if CD_FIELDS <= set(fields) and (
                fields["cd_state"] not in ("IDLE", "ACTIVE", "HOLDING", "FAULT_LATCHED") or
                fields["cd_release_owed"] not in ("0", "1") or
                fields["cd_lease_expired"] not in ("0", "1")):
            raise ValueError("invalid CD status")
        if RECEIVER_FIELDS <= set(fields):
            if (fields["receiver_source"] not in SOURCES + ("UNKNOWN",) or
                    fields["receiver_monitor"] not in ("ON", "OFF", "UNKNOWN") or
                    fields["receiver_integrity"] not in ("HEALTHY", "UNCERTAIN") or
                    fields["receiver_source_evidence"] not in
                    ("NONE", "PASSIVE", "AFTER_LOCAL_TX", "QUERY") or
                    fields["receiver_monitor_evidence"] not in
                    ("NONE", "PASSIVE", "AFTER_LOCAL_TX", "QUERY") or
                    not fields["receiver_source_ns"].isdecimal() or
                    not fields["receiver_monitor_ns"].isdecimal()):
                raise ValueError("invalid receiver state")
            for name in ("source", "monitor"):
                unknown = fields[f"receiver_{name}"] == "UNKNOWN"
                empty = (fields[f"receiver_{name}_ns"] == "0" and
                         fields[f"receiver_{name}_evidence"] == "NONE")
                if unknown != empty or (not unknown and
                        (fields[f"receiver_{name}_ns"] == "0" or
                         fields[f"receiver_{name}_evidence"] == "NONE")):
                    raise ValueError("inconsistent receiver observation")
        if CD_PRESENCE_FIELDS <= set(fields):
            presence, last = fields["cd_changer"], fields["cd_changer_last_probe"]
            observed_ns = fields["cd_changer_ns"]
            # Canonical unsigned 64-bit decimal: no sign, padding or leading zeros.
            if (presence not in CD_PRESENCE_VALUES or last not in CD_PROBE_RESULTS or
                    not observed_ns.isdecimal() or
                    observed_ns != str(int(observed_ns)) or int(observed_ns) >= 1 << 64):
                raise ValueError("invalid CD presence")
            # UNKNOWN exactly when nothing was observed; a RESPONSE or NO_RESPONSE
            # always sets the value it implies, and no probe means no observation.
            if ((presence == "UNKNOWN") != (observed_ns == "0") or
                    (last == "NONE" and presence != "UNKNOWN") or
                    (last == "RESPONSE" and presence != "PRESENT") or
                    (last == "NO_RESPONSE" and presence != "NOT_DETECTED")):
                raise ValueError("inconsistent CD presence")
        if not RECEIVER_FIELDS <= set(fields):
            fields.update({"receiver_source": "UNKNOWN", "receiver_source_ns": "0",
                           "receiver_source_evidence": "NONE",
                           "receiver_monitor": "UNKNOWN", "receiver_monitor_ns": "0",
                           "receiver_monitor_evidence": "NONE",
                           "receiver_integrity": "UNCERTAIN"})
    except (UnicodeError, ValueError) as exc:
        raise ServiceError("Invalid XS8 service STATUS reply") from exc
    return fields


def receiver_state(status):
    def field(name):
        observed_ns = int(status[f"receiver_{name}_ns"])
        age_ms = max(0, (time.monotonic_ns() - observed_ns) // 1_000_000) if observed_ns else None
        return {"value": status[f"receiver_{name}"], "observed_mono_ns": observed_ns,
                "evidence": status[f"receiver_{name}_evidence"], "age_ms": age_ms,
                "freshness": ("UNKNOWN" if not observed_ns else
                              "UNCERTAIN" if status["receiver_integrity"] != "HEALTHY" else
                              "FRESH" if age_ms <= 30000 else "LAST_KNOWN")}
    return {"source": field("source"), "monitor": field("monitor"),
            "integrity": status["receiver_integrity"],
            "receiver_execution": "UNVERIFIED"}


def cd_changer_state(status):
    """Diagnostic CD changer presence from STATUS; None from a service without it."""
    if "cd_changer" not in status:
        return None
    observed_ns = int(status["cd_changer_ns"])
    age_ms = max(0, (time.monotonic_ns() - observed_ns) // 1_000_000) if observed_ns else None
    return {"value": status["cd_changer"], "observed_mono_ns": observed_ns,
            "age_ms": age_ms, "last_probe": status["cd_changer_last_probe"],
            "diagnostic": True, "receiver_execution": "UNVERIFIED"}


def capabilities(mute_available=False, source_available=False, query_available=False,
                 cd_track_available=False, cd_track_interval_ms=CD_TRACK_INTERVAL_MS,
                 volume_step_available=False, cd_playback_available=False,
                 cd_seek_available=False, cd_blocked_actions=CD_BLOCKED_ACTIONS,
                 volume_hold_available=False, power_available=False,
                 tape2_monitor_available=False, power_toggle_available=False,
                 cd_track_select_available=False):
    return {
        "api_version": "v0.1",
        "operations": {
            "status": {"classification": "supported", "enabled": True},
            "receiver_query": {"classification": "supported", "enabled": query_available},
            "mute": {
                "classification": "unverified", "enabled": mute_available,
                "reason": "Receiver execution is unverified",
            },
            "volume_step": {
                "classification": "unverified", "enabled": volume_step_available,
                "available_directions": list(VOLUME_DIRECTIONS),
                "min_interval_ms": VOLUME_STEP_INTERVAL_MS,
                "reason": "TX and waveform do not confirm a volume change; hold and repeat are not exposed",
            },
            "source": {
                "classification": "unverified", "enabled": source_available,
                "available_sources": list(EVIDENCED_SOURCES),
                "blocked_sources": [s for s in SOURCES if s not in EVIDENCED_SOURCES],
                "reason": "TX and waveform do not confirm receiver execution; TAPE2 is a MONITOR, not a source selection",
            },
            "cd_track_select": {
                "classification": "unverified", "enabled": cd_track_select_available,
                "min_track": CD_TRACK_SELECT_RANGE[0],
                "max_track": CD_TRACK_SELECT_RANGE[1],
                "frame_gap_ms": CD_TRACK_SELECT_GAP_MS,
                "min_interval_ms": cd_track_interval_ms,
                "reason": "Track N is floor(N/10) ROOT 0xDA then one digit frame; 1-39 is an operational limit; TX and waveform do not confirm the track",
            },
            "cd_transport": {
                "classification": "unverified", "enabled": cd_track_available,
                "available_actions": list(CD_TRACK_ACTIONS),
                "blocked_actions": list(cd_blocked_actions),
                "min_interval_ms": cd_track_interval_ms,
                "reason": "TX and waveform do not confirm a track change; blocked actions are not exposed by this Gateway",
            },
            "cd_playback": {
                "classification": "unverified", "enabled": cd_playback_available,
                "available_actions": list(CD_PLAYBACK_ACTIONS),
                "provisional_actions": list(CD_PROVISIONAL_ACTIONS),
                "min_interval_ms": cd_track_interval_ms,
                "reason": "STOP 0xA1, TIME_DISPLAY 0xEA, REPEAT 0xCC and DISC_SKIP 0x8A are physically observed; PLAY/PAUSE 0xBC is a historical mapping not yet physically verified; TX and waveform do not confirm a changer action",
            },
            "volume_hold": {
                "classification": "unverified", "enabled": volume_hold_available,
                "available_directions": list(VOLUME_HOLD_DIRECTIONS),
                "hold_delay_ms": VOLUME_HOLD_DELAY_MS,
                "heartbeat_ms": CD_SEEK_HEARTBEAT_MS, "lease_ms": CD_SEEK_LEASE_MS,
                "max_hold_ms": VOLUME_HOLD_MAX_MS,
                "reason": "Held VOL+/VOL- on a service connection; a short press stays one VOLUME_STEP",
            },
            "power": {
                "classification": "unverified", "enabled": power_available,
                "available_actions": list(POWER_COMMANDS),
                "provisional_actions": list(POWER_PROVISIONAL_ACTIONS),
                # TOGGLE needs fresh receiver queries, so also --enable-receiver-query.
                "toggle_available": power_toggle_available,
                "reason": "ROOT 0x52 ON and ROOT 0x62 OFF are physically verified; TX does not confirm execution and no power state is read back",
            },
            "tape2_monitor": {
                "classification": "unverified", "enabled": tape2_monitor_available,
                "provisional": True,
                "reason": "CONTEXT 0x48 is a historical MONITOR toggle, not yet physically validated; the indicator shows only observed receiver reports",
            },
            "cd_seek": {
                "classification": "unverified", "enabled": cd_seek_available,
                "available_directions": list(CD_SEEK_DIRECTIONS),
                "heartbeat_ms": CD_SEEK_HEARTBEAT_MS, "lease_ms": CD_SEEK_LEASE_MS,
                "max_hold_ms": CD_SEEK_MAX_HOLD_MS,
                "reason": "Held KEY DOWN with a service lease and maximum; TX and waveform do not confirm seeking",
            },
        },
    }


def cd_seek_state(server):
    # Server-wide: the service allows one CD hold. Its owning service connection
    # stays here, bound to the session that started it; browsers see only hold_id.
    return server.__dict__.setdefault("cd_seek", {"hold": None})


def end_cd_seek(server, reason):
    """Forget the hold and close its service connection; the service releases
    on close. Idempotent."""
    registry = cd_seek_state(server)
    hold = registry["hold"]
    registry["hold"] = None
    if hold is not None:
        hold["connection"].close()
        print(f"XS8 gateway CD_SEEK connection closed reason={reason}", file=sys.stderr)


class CdSeekCleanupMixin:
    """Closes abandoned seek connections from the serve_forever loop; no threads."""

    def service_actions(self):
        super().service_actions()
        hold = cd_seek_state(self)["hold"]
        if hold is None:
            return
        now = time.monotonic()
        sessions = getattr(self.RequestHandlerClass, "sessions", None) or {}
        session = hold["session"]
        if hold["connection"].closed:
            end_cd_seek(self, "CONNECTION_LOST")
        elif session["expires"] < now or not any(s is session for s in sessions.values()):
            end_cd_seek(self, "SESSION_ENDED")
        elif (now - hold["browser_seen"]) * 1000 >= CD_SEEK_BROWSER_TIMEOUT_MS:
            end_cd_seek(self, "BROWSER_INACTIVE")
        elif (now - hold["started"]) * 1000 >= hold["max_ms"]:
            end_cd_seek(self, "MAX_HOLD")

    def server_close(self):
        end_cd_seek(self, "GATEWAY_SHUTDOWN")
        super().server_close()


class GatewayHTTPServer(CdSeekCleanupMixin, HTTPServer):
    pass


class GatewayHandler(BaseHTTPRequestHandler):
    socket_path = DEFAULT_SOCKET
    dev_mute_enabled = False
    production_mute_enabled = False
    production_source_enabled = False
    receiver_query_enabled = False
    cd_track_enabled = False
    cd_track_interval_ms = CD_TRACK_INTERVAL_MS
    volume_step_enabled = False
    cd_playback_enabled = False
    cd_seek_enabled = False
    volume_hold_enabled = False
    power_enabled = False
    tape2_monitor_enabled = False
    cd_track_select_enabled = False
    https_origin = None
    sessions = None

    def mute_enabled(self):
        return self.dev_mute_enabled or self.production_mute_enabled

    def control_enabled(self):
        return (self.mute_enabled() or self.production_source_enabled or
                self.receiver_query_enabled or self.cd_track_enabled or
                self.volume_step_enabled or self.cd_playback_enabled or
                self.cd_seek_enabled or self.volume_hold_enabled or
                self.power_enabled or self.tape2_monitor_enabled or
                self.cd_track_select_enabled)

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

    def service_status(self):
        try:
            return self.read_service_status()
        except ServiceError:
            return None

    def start_timing(self):
        self.request_started = time.monotonic()
        self.timings = []

    def add_timing(self, name, started):
        if getattr(self, "timings", None) is not None:
            self.timings.append((name, (time.monotonic() - started) * 1000))

    def read_service_status(self):
        started = time.monotonic()
        try:
            return read_status(self.socket_path)
        finally:
            self.add_timing("xs8status", started)

    @staticmethod
    def status_allows_mute(status):
        return (status["state"] == "IDLE" and status["monitor"] == "HEALTHY"
                and status["stop_owed"] == "0" and status["context_may_be_open"] == "0"
                and status.get("cd_state", "IDLE") == "IDLE"
                and status.get("cd_release_owed", "0") == "0")

    @classmethod
    def status_allows_cd_track(cls, status):
        # A service without CD STATUS fields cannot report a latched CD fault.
        return "cd_state" in status and cls.status_allows_mute(status)

    @classmethod
    def status_allows_volume_step(cls, status):
        # Like CD: only a service reporting CD fields can report a latched CD fault.
        return "cd_state" in status and cls.status_allows_mute(status)

    @staticmethod
    def cd_fault(status):
        # An active pair or hold legitimately owes RELEASE; an idle one does not.
        return (status.get("cd_state") == "FAULT_LATCHED" or
                (status.get("cd_state") == "IDLE" and
                 status.get("cd_release_owed") == "1"))

    def availability(self, status):
        ready = status is not None and self.status_allows_mute(status)
        return {"mute": self.mute_enabled() and ready,
                "source": self.production_source_enabled and ready,
                "query": self.receiver_query_enabled and ready,
                "cd_track": (self.cd_track_enabled and status is not None and
                             self.status_allows_cd_track(status)),
                "volume_step": (self.volume_step_enabled and status is not None and
                                self.status_allows_volume_step(status)),
                "cd_playback": (self.cd_playback_enabled and status is not None and
                                self.status_allows_cd_track(status)),
                "cd_seek": (self.cd_seek_enabled and status is not None and
                            self.status_allows_cd_track(status)),
                "volume_hold": (self.volume_hold_enabled and status is not None and
                                self.status_allows_volume_step(status)),
                "power": (self.power_enabled and status is not None and
                          self.status_allows_cd_track(status)),
                "tape2_monitor": (self.tape2_monitor_enabled and status is not None and
                                  self.status_allows_volume_step(status)),
                "cd_track_select": (self.cd_track_select_enabled and status is not None and
                                    self.status_allows_cd_track(status))}

    def cd_blocked_actions(self):
        enabled = ((CD_PLAYBACK_ACTIONS if self.cd_playback_enabled else ()) +
                   (("SEEK_FORWARD", "SEEK_BACKWARD") if self.cd_seek_enabled else ()))
        return tuple(a for a in CD_BLOCKED_ACTIONS if a not in enabled)

    def send_body(self, code, body, content_type, headers=None):
        self.response_code = code
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'")
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        if getattr(self, "timings", None) is not None:
            # Diagnostics only: lets the browser separate Gateway/XS8 time from network.
            entries = self.timings + [
                ("total", (time.monotonic() - self.request_started) * 1000)]
            self.send_header("Server-Timing", ", ".join(
                f"{name};dur={duration:.1f}" for name, duration in entries))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, code, payload, headers=None):
        self.response_result = payload.get("result")
        self.send_body(code, json.dumps(payload, separators=(",", ":")).encode(),
                       "application/json; charset=utf-8", headers)

    def do_GET(self):
        self.start_timing()
        path = urlsplit(self.path).path
        if path == "/api/v1/status":
            try:
                status = self.read_service_status()
            except ServiceError as exc:
                self.send_json(503, {"gateway": "online", "xs8_service": "unavailable",
                                     "error": str(exc)})
                return
            self.send_json(200, {"gateway": "online", "xs8_service": "connected",
                                 "status": status, "receiver": receiver_state(status),
                                 "cd_changer": cd_changer_state(status),
                                 "power": self.power_observation().snapshot()})
            return
        if path == "/api/v1/capabilities":
            status = self.service_status() if self.current_session() is not None else None
            available = self.availability(status)
            self.send_json(200, capabilities(available["mute"], available["source"],
                                              available["query"], available["cd_track"],
                                              self.cd_track_interval_ms,
                                              available["volume_step"],
                                              available["cd_playback"], available["cd_seek"],
                                              self.cd_blocked_actions(),
                                              available["volume_hold"], available["power"],
                                              available["tape2_monitor"],
                                              power_toggle_available=available["power"] and
                                              self.receiver_query_enabled,
                                              cd_track_select_available=available[
                                                  "cd_track_select"]))
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
            available = self.availability(self.service_status())
            self.send_json(200, {"csrf_token": session["csrf"],
                                 "mute_available": available["mute"],
                                 "source_available": available["source"],
                                 "receiver_query_available": available["query"],
                                 "cd_track_available": available["cd_track"],
                                 "volume_step_available": available["volume_step"],
                                 "cd_playback_available": available["cd_playback"],
                                 "cd_seek_available": available["cd_seek"],
                                 "volume_hold_available": available["volume_hold"],
                                 "power_available": available["power"],
                                 "tape2_monitor_available": available["tape2_monitor"],
                                 "cd_track_select_available": available["cd_track_select"]},
                           headers)
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

    def cd_track_state(self):
        # Server-wide, not per session: spacing protects the one changer.
        return self.server.__dict__.setdefault(
            "cd_track", {"lock": threading.Lock(), "completed": float("-inf")})

    def dispatch_cd_track(self, action):
        started = time.monotonic()
        try:
            return send_cd_track(self.socket_path, action)
        finally:
            # Any dispatch outcome, including timeout, starts the interval.
            self.cd_track_state()["completed"] = time.monotonic()
            self.add_timing("xs8cd", started)

    def dispatch_cd_track_select(self, track):
        started = time.monotonic()
        try:
            return send_cd_track_select(self.socket_path, track)
        finally:
            # Any outcome, including a timeout, starts the shared CD interval.
            self.cd_track_state()["completed"] = time.monotonic()
            self.add_timing("xs8cd", started)

    def dispatch_cd_playback(self, action):
        started = time.monotonic()
        try:
            return send_cd_playback(self.socket_path, action)
        finally:
            self.cd_track_state()["completed"] = time.monotonic()
            self.add_timing("xs8cd", started)

    def cd_seek_state(self):
        return cd_seek_state(self.server)

    def forget_stale_hold(self, status):
        # STATUS without that hold means the service already ended it (inactivity,
        # maximum, monitor). Closing the dead connection also clears the service's
        # re-arm block for it.
        hold = self.cd_seek_state()["hold"]
        if hold is None:
            return
        if (status.get("cd_state") == "IDLE" if hold["kind"] == "cd" else
                status.get("state") not in ("HOLDING_UP", "HOLDING_DOWN")):
            end_cd_seek(self.server, "SERVICE_IDLE")

    def take_source_locks(self, names=SOURCE_CONTROL_LOCKS):
        """Hold every named control lock for one request, or none (refused)."""
        taken = []
        for name in names:
            lock = self.server.__dict__.setdefault(
                name, {"lock": threading.Lock(), "completed": float("-inf")})["lock"]
            if not lock.acquire(blocking=False):
                for held in taken:
                    held.release()
                return False
            taken.append(lock)
        self.extra_locks = taken
        return True

    def dispatch_sync(self, session, requested=None, expected_monitor=None):
        """The final SYNC after a verified source or MONITOR control: one
        QUERY_STATUS SETTLE, never retried, its outcome reported apart from the
        control. Indicators change only from the observation the service records;
        since_mono_ns lets the browser ignore anything observed before the control."""
        if not self.receiver_query_enabled:
            return {"result": "NOT_ENABLED"}
        # Counts as this session's query for the manual SYNC spacing.
        session["last_query_request"] = time.monotonic()
        started = time.monotonic()
        sync = {"since_mono_ns": self.control_started_ns}
        try:
            sync.update(self.observed_query(settle=True))
        except MuteServiceError as exc:
            sync.update({"result": exc.result, "error": str(exc)})
            if exc.evidence:
                sync.update(exc.evidence)
        finally:
            self.add_timing("xs8sync", started)
        if requested is not None:
            observed = sync.get("observed_source")
            sync["requested"] = requested
            sync["match"] = observed == requested if observed else None
        else:
            observed = sync.get("observed_monitor")
            # Expected only when the MONITOR state before the toggle was known.
            sync["expected_monitor"] = expected_monitor
            sync["match"] = (observed == expected_monitor
                             if observed and expected_monitor else None)
        return sync

    def power_observation(self):
        return self.server.__dict__.setdefault("power_observation", PowerObservation())

    def observed_query(self, settle):
        """One QUERY_STATUS whose outcome also feeds the diagnostic power observation."""
        try:
            reply = send_receiver_query(self.socket_path, settle=settle)
        except MuteServiceError as exc:
            self.power_observation().record(exc, settle)
            raise
        self.power_observation().record(reply, settle)
        return reply

    def power_refusal(self, status):
        """Why a POWER frame may not be sent now, or None."""
        return ("CD_FAULT_LATCHED" if self.cd_fault(status) else
                "FAULT_LATCHED" if status.get("state") == "FAULT_LATCHED" else
                "CD_BUSY" if status.get("cd_state") in ("ACTIVE", "HOLDING") else
                "VOLUME_BUSY" if status.get("state") in ("HOLDING_UP", "HOLDING_DOWN") else
                None if self.status_allows_cd_track(status) else "NOT_ELIGIBLE")

    def dispatch_power_toggle(self, spacing, expected):
        """One-click POWER: decide from fresh QUERY_STATUS SETTLE observations made
        under every control lock, then send at most one POWER frame, never retried.
        Recognised reply: ON -> POWER_OFF. Two clean NO_REPLY results at least 1 s
        apart: OFF_INFERRED -> POWER_ON. Anything else sends nothing. Direct service
        clients and the receiver's own controls can still act between the last
        observation and the POWER frame; this is not globally atomic."""
        observations = []

        def observe():
            started = time.monotonic()
            try:
                self.observed_query(settle=True)
                observations.append({"result": "OBSERVATION_RECORDED"})
                return "REPLY"
            except MuteServiceError as exc:
                entry = {"result": exc.result}
                if exc.evidence and exc.evidence.get("reason"):
                    entry["reason"] = exc.evidence["reason"]
                observations.append(entry)
                return "NO_REPLY" if entry == {"result": "OBSERVATION_FAILED",
                                               "reason": "NO_REPLY"} else "FAULT"
            finally:
                self.add_timing("xs8powerquery", started)

        first = observe()
        state = "ON" if first == "REPLY" else "UNKNOWN"
        if first == "NO_REPLY":
            time.sleep(POWER_SILENCE_GAP_S)
            state = "OFF_INFERRED" if observe() == "NO_REPLY" else "UNKNOWN"
        decision = {"state": state, "observations": observations}
        refused = {"decision": decision, "power_sent": False}
        if state == "UNKNOWN":
            raise PowerToggleRefused("POWER_STATE_UNKNOWN",
                                     "Receiver power could not be observed reliably", refused)
        # Without expected (the browser showed UNKNOWN or a stale state, for
        # example right after a POWER frame or a Gateway restart) the fresh
        # observation alone decides.
        if expected is not None and state != expected:
            raise PowerToggleRefused("STATE_CHANGED",
                                     f"Receiver power observed {state}, expected {expected}",
                                     refused)
        # Eligibility again immediately before TX; no other frame since our query.
        try:
            status = self.read_service_status()
        except ServiceError as exc:
            raise PowerToggleRefused("UNAVAILABLE", "XS8 service unavailable", refused) from exc
        refusal = self.power_refusal(status)
        if refusal is None and status.get("last_byte") != "0x13":
            refusal = "INTERVENING_TX"
        if refusal:
            raise PowerToggleRefused(refusal, "POWER no longer eligible", refused)
        command = "OFF" if state == "ON" else "ON"
        self.power_observation().invalidate()
        try:
            reply = self.dispatch_spaced(spacing, send_power, command)
        except MuteServiceError as exc:
            exc.evidence = dict(exc.evidence or {}, decision=decision, sent=command,
                                power_sent=True)
            raise
        return dict(reply, decision=decision, sent=command, power_sent=True)

    @staticmethod
    def source_is_fresh_cd(status):
        # Only a fresh observation with healthy integrity; UNKNOWN, LAST_KNOWN
        # and UNCERTAIN never cause a STOP.
        source = receiver_state(status)["source"]
        return source["value"] == "CD" and source["freshness"] == "FRESH"

    def dispatch_source(self, source, stop_cd):
        """SELECT_SOURCE, then CD_STOP when leaving CD. A failed or uncertain
        SELECT_SOURCE raises before CD_STOP; neither is ever retried. No delay is
        added between them."""
        self.source_stage = "SELECT_SOURCE"
        reply = send_source(self.socket_path, source)
        if stop_cd:
            self.source_evidence = reply
            self.source_stage = "CD_STOP"
            self.cd_stop_evidence = self.dispatch_cd_playback("STOP")
        return reply

    def take_spacing(self, name, interval_ms, session):
        """Server-wide lock and completion spacing for one control, or None after
        a refusal (the token is consumed)."""
        state = self.server.__dict__.setdefault(
            name, {"lock": threading.Lock(), "completed": float("-inf")})
        if not state["lock"].acquire(blocking=False):
            session["csrf"] = secrets.token_urlsafe(32)
            self.send_json(409, {"result": "BUSY", "receiver_execution": "UNVERIFIED"})
            return None
        self.held_lock = state["lock"]
        if (time.monotonic() - state["completed"]) * 1000 < interval_ms:
            session["csrf"] = secrets.token_urlsafe(32)
            self.send_json(429, {"result": "TOO_FAST", "min_interval_ms": interval_ms,
                                 "receiver_execution": "UNVERIFIED"})
            return None
        return state

    def dispatch_explicit_power(self, spacing, action):
        # Any POWER transmission makes the power observation unknown.
        self.power_observation().invalidate()
        return self.dispatch_spaced(spacing, send_power, action)

    def dispatch_spaced(self, state, send, *args):
        started = time.monotonic()
        try:
            return send(self.socket_path, *args)
        finally:
            state["completed"] = time.monotonic()
            self.add_timing("xs8tx", started)

    def volume_step_state(self):
        # Server-wide, like CD: one receiver, whichever session sends.
        return self.server.__dict__.setdefault(
            "volume_step", {"lock": threading.Lock(), "completed": float("-inf")})

    def dispatch_volume_step(self, direction):
        started = time.monotonic()
        try:
            return send_volume_step(self.socket_path, direction)
        finally:
            # Any dispatch outcome, including timeout, starts the interval.
            self.volume_step_state()["completed"] = time.monotonic()
            self.add_timing("xs8volume", started)

    def do_POST(self):
        self.start_timing()
        self.held_lock = None
        self.extra_locks = []
        self.source_stage = None
        self.source_evidence = None
        self.cd_stop_evidence = None
        self.cd_action = None
        self.cd_kind = "CD_TRACK"
        self.volume_direction = None
        try:
            self.handle_post()
        finally:
            if self.held_lock is not None:
                self.held_lock.release()
            for lock in self.extra_locks:
                lock.release()
            if self.volume_direction is not None:
                self.log_message("VOLUME_STEP direction=%s code=%s result=%s %s",
                                 self.volume_direction,
                                 getattr(self, "response_code", "-"),
                                 getattr(self, "response_result", None) or "-",
                                 " ".join(f"{name}_ms={duration:.1f}"
                                          for name, duration in self.timings))
            if self.cd_action is not None:
                # One journal line per CD request for attended timing analysis.
                self.log_message("%s action=%s code=%s result=%s %s", self.cd_kind,
                                 self.cd_action,
                                 getattr(self, "response_code", "-"),
                                 getattr(self, "response_result", None) or "-",
                                 " ".join(f"{name}_ms={duration:.1f}"
                                          for name, duration in self.timings))

    def handle_post(self):
        path = urlsplit(self.path).path
        is_mute = path == "/api/v1/mute" and self.mute_enabled()
        is_source = path == "/api/v1/source" and self.production_source_enabled
        is_query = path == "/api/v1/receiver/query" and self.receiver_query_enabled
        is_cd = path == "/api/v1/cd/track" and self.cd_track_enabled
        # Direct track entry has its own default-off gate; it shares the CD track
        # locks, spacing and checks.
        is_select = path == "/api/v1/cd/track/select" and self.cd_track_select_enabled
        is_volume = path == "/api/v1/volume/step" and self.volume_step_enabled
        is_playback = path == "/api/v1/cd/playback" and self.cd_playback_enabled
        is_seek = path == "/api/v1/cd/seek" and self.cd_seek_enabled
        is_volume_hold = path == "/api/v1/volume/hold" and self.volume_hold_enabled
        is_power = path == "/api/v1/power" and self.power_enabled
        is_toggle = False
        is_monitor = path == "/api/v1/receiver/monitor" and self.tape2_monitor_enabled
        if (not (is_mute or is_source or is_query or is_cd or is_select or is_volume or
                 is_playback or is_seek or is_volume_hold or is_power or is_monitor) or
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
                ((is_mute or is_query or is_monitor) and lengths[0] != "2") or
                types != ["application/json"] or self.headers.get("Transfer-Encoding")):
            self.send_json(400, {"error": "Invalid JSON request headers"})
            return
        self.connection.settimeout(2)
        try:
            body = self.rfile.read(int(lengths[0]))
        except (OSError, TimeoutError):
            return
        if is_mute or is_query or is_monitor:
            if body != b"{}":
                self.send_json(400, {"error": "Expected JSON body {}"})
                return
            if is_monitor:
                self.cd_kind, self.cd_action = "TAPE2_MONITOR", "TOGGLE"
                spacing = self.take_spacing("tape2_monitor", TAPE2_MONITOR_INTERVAL_MS, session)
                if spacing is None:
                    return
                if not self.take_source_locks(MONITOR_CONTROL_LOCKS):
                    session["csrf"] = secrets.token_urlsafe(32)
                    self.send_json(409, {"result": "BUSY", "receiver_execution": "UNVERIFIED"})
                    return
        elif is_seek or is_volume_hold:
            kind = "cd" if is_seek else "volume"
            request = self.parse_seek(body, kind)
            if request is None:
                self.send_json(400, {"error": "Expected one allowlisted hold request"})
                return
            self.cd_kind = KEY_HOLDS[kind]["label"]
            self.cd_action = request["action"]
            self.handle_seek(session, request, kind)
            return
        elif is_power:
            # {"action": "ON"|"OFF"}, or {"action": "TOGGLE"} with an optional
            # "expected": "ON"|"OFF_INFERRED", the fresh state the browser showed.
            # expected only guards; the fresh observation always decides.
            try:
                pairs = json.loads(body, object_pairs_hook=list)
                fields = (dict(pairs) if isinstance(pairs, list) and
                          all(isinstance(p, tuple) for p in pairs) and
                          len(dict(pairs)) == len(pairs) else {})
            except (UnicodeError, ValueError, TypeError):
                fields = {}
            action = fields.get("action")
            expected = fields.get("expected")
            if not isinstance(action, str) or not (
                    (set(fields) == {"action"} and
                     (action in POWER_COMMANDS or action == "TOGGLE")) or
                    (set(fields) == {"action", "expected"} and action == "TOGGLE" and
                     expected in POWER_TOGGLE_STATES)):
                self.send_json(400, {"error": "Expected one allowlisted power action"})
                return
            is_toggle = action == "TOGGLE"
            self.cd_kind, self.cd_action = "POWER", action
            spacing = self.take_spacing("power", POWER_INTERVAL_MS, session)
            if spacing is None:
                return
            if is_toggle and not self.receiver_query_enabled:
                session["csrf"] = secrets.token_urlsafe(32)
                self.send_json(409, {"result": "TOGGLE_UNAVAILABLE", "power_sent": False,
                                     "receiver_execution": "UNVERIFIED"})
                return
            # TOGGLE holds every control lock through its queries and POWER frame.
            if is_toggle and not self.take_source_locks(
                    ("cd_track", "volume_step", "tape2_monitor")):
                session["csrf"] = secrets.token_urlsafe(32)
                self.send_json(409, {"result": "BUSY", "power_sent": False,
                                     "receiver_execution": "UNVERIFIED"})
                return
        elif is_cd or is_playback or is_select:
            key = "track" if is_select else "action"
            try:
                pairs = json.loads(body, object_pairs_hook=list)
                value = (pairs[0][1] if isinstance(pairs, list) and
                         len(pairs) == 1 and isinstance(pairs[0], tuple) and
                         pairs[0][0] == key else None)
            except (UnicodeError, ValueError, TypeError, IndexError):
                value = None
            if is_select:
                # Exactly {"track": N}: a JSON integer in range, never a string,
                # float, boolean, byte value or list.
                if (not isinstance(value, int) or isinstance(value, bool) or
                        not CD_TRACK_SELECT_RANGE[0] <= value <= CD_TRACK_SELECT_RANGE[1]):
                    self.send_json(400, {"error": "Expected one track number from %d to %d"
                                         % CD_TRACK_SELECT_RANGE})
                    return
                track, action = value, "SELECT"
                self.cd_kind = "CD_TRACK_SELECT"
                # The rest of the request follows the CD track path exactly.
                is_cd = True
            elif not isinstance(value, str) or value not in (
                    CD_TRACK_ACTIONS if is_cd else CD_PLAYBACK_ACTIONS):
                self.send_json(400, {"error": "Expected one allowlisted CD action"})
                return
            else:
                action = value
            if is_playback:
                self.cd_kind = "CD_PLAYBACK"
            self.cd_action = str(track) if is_select else action
            state = self.cd_track_state()
            if state["completed"] != float("-inf"):
                # Not work time: how long since the previous CD dispatch completed.
                self.timings.append(("sincecd", (time.monotonic() - state["completed"]) * 1000))
            if not state["lock"].acquire(blocking=False):
                session["csrf"] = secrets.token_urlsafe(32)
                self.send_json(409, {"result": "CD_BUSY", "receiver_execution": "UNVERIFIED"})
                return
            self.held_lock = state["lock"]
            if (self.cd_track_interval_ms != CD_TRACK_INTERVAL_OFF and
                    (time.monotonic() - state["completed"]) * 1000 < self.cd_track_interval_ms):
                session["csrf"] = secrets.token_urlsafe(32)
                self.send_json(429, {"result": "TOO_FAST",
                                     "min_interval_ms": self.cd_track_interval_ms,
                                     "receiver_execution": "UNVERIFIED"})
                return
        elif is_volume:
            try:
                pairs = json.loads(body, object_pairs_hook=list)
                direction = (pairs[0][1] if isinstance(pairs, list) and
                             len(pairs) == 1 and isinstance(pairs[0], tuple) and
                             pairs[0][0] == "direction" else None)
            except (UnicodeError, ValueError, TypeError, IndexError):
                direction = None
            if not isinstance(direction, str) or direction not in VOLUME_DIRECTIONS:
                self.send_json(400, {"error": "Expected one allowlisted volume direction"})
                return
            self.volume_direction = direction
            state = self.volume_step_state()
            if not state["lock"].acquire(blocking=False):
                session["csrf"] = secrets.token_urlsafe(32)
                self.send_json(409, {"result": "VOLUME_BUSY", "receiver_execution": "UNVERIFIED"})
                return
            self.held_lock = state["lock"]
            if (time.monotonic() - state["completed"]) * 1000 < VOLUME_STEP_INTERVAL_MS:
                session["csrf"] = secrets.token_urlsafe(32)
                self.send_json(429, {"result": "TOO_FAST",
                                     "min_interval_ms": VOLUME_STEP_INTERVAL_MS,
                                     "receiver_execution": "UNVERIFIED"})
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
        if is_query:
            now = time.monotonic()
            if now - session.get("last_query_request", float("-inf")) < 2.0:
                session["csrf"] = secrets.token_urlsafe(32)
                self.send_json(429, {"result": "TOO_FAST",
                                     "receiver_execution": "UNVERIFIED"})
                return
            session["last_query_request"] = now
        # Single use: a repeated request with the same token cannot dispatch again.
        session["csrf"] = secrets.token_urlsafe(32)
        if is_source and not self.take_source_locks():
            self.send_json(409, {"result": "BUSY", "receiver_execution": "UNVERIFIED"})
            return
        stop_cd = False
        if (is_source or is_query or is_cd or is_volume or is_playback or
                is_power or is_monitor or self.production_mute_enabled):
            try:
                status = self.read_service_status()
            except ServiceError:
                self.send_json(503, {"result": "UNAVAILABLE", "receiver_execution": "UNVERIFIED"})
                return
            self.forget_stale_hold(status)
            if (is_cd or is_playback) and self.cd_fault(status):
                self.send_json(409, {"result": "CD_FAULT_LATCHED",
                                     "physical_check_required": True,
                                     "receiver_execution": "UNVERIFIED"})
                return
            if (is_cd or is_playback) and status.get("cd_state") in ("ACTIVE", "HOLDING"):
                self.send_json(409, {"result": "CD_BUSY", "receiver_execution": "UNVERIFIED"})
                return
            if is_volume or is_power or is_monitor:
                refusal = ("CD_FAULT_LATCHED" if self.cd_fault(status) else
                           "FAULT_LATCHED" if status.get("state") == "FAULT_LATCHED" else
                           "CD_BUSY" if status.get("cd_state") in ("ACTIVE", "HOLDING") else
                           "VOLUME_BUSY" if status.get("state") in ("HOLDING_UP", "HOLDING_DOWN")
                           else None)
                if refusal:
                    payload = {"result": refusal, "receiver_execution": "UNVERIFIED"}
                    if refusal.endswith("FAULT_LATCHED"):
                        payload["physical_check_required"] = True
                    self.send_json(409, payload)
                    return
            if not (self.status_allows_cd_track(status) if (is_cd or is_playback or is_power)
                    else self.status_allows_volume_step(status) if (is_volume or is_monitor)
                    else self.status_allows_mute(status)):
                self.send_json(409, {"result": "NOT_ELIGIBLE", "receiver_execution": "UNVERIFIED"})
                return
            stop_cd = (is_source and source in SOURCES_STOPPING_CD and
                       self.source_is_fresh_cd(status))
            if stop_cd:
                self.cd_kind, self.cd_action = "SOURCE_CD_STOP", source
                # The STOP is a CD dispatch: CD STATUS fields and CD spacing apply.
                interval = self.cd_track_interval_ms
                if not self.status_allows_cd_track(status):
                    self.send_json(409, {"result": "NOT_ELIGIBLE", "source": source,
                                         "receiver_execution": "UNVERIFIED"})
                    return
                if (interval != CD_TRACK_INTERVAL_OFF and (time.monotonic() -
                        self.cd_track_state()["completed"]) * 1000 < interval):
                    self.send_json(429, {"result": "TOO_FAST", "min_interval_ms": interval,
                                         "source": source, "receiver_execution": "UNVERIFIED"})
                    return
        if is_monitor:
            before = receiver_state(status)["monitor"]
            expected_monitor = ({"ON": "OFF", "OFF": "ON"}.get(before["value"])
                                if before["freshness"] == "FRESH" else None)
        self.control_started_ns = time.monotonic_ns()
        try:
            reply = (send_mute(self.socket_path, fake_only=not self.production_mute_enabled)
                     if is_mute else self.observed_query(settle=False) if is_query
                     else self.dispatch_cd_track_select(track) if is_select
                     else self.dispatch_cd_track(action) if is_cd
                     else self.dispatch_cd_playback(action) if is_playback
                     else self.dispatch_volume_step(direction) if is_volume
                     else self.dispatch_power_toggle(spacing, expected) if is_toggle
                     else self.dispatch_explicit_power(spacing, action) if is_power
                     else self.dispatch_spaced(spacing, send_tape2_monitor) if is_monitor
                     else self.dispatch_source(source, stop_cd))
        except MuteServiceError as exc:
            code = {"UNAVAILABLE": 503, "REJECTED": 409, "CD_FAULT_LATCHED": 409,
                    "FAULT_LATCHED": 409, "TIMEOUT": 504}.get(exc.result, 502)
            if isinstance(exc, PowerToggleRefused):
                code = 503 if exc.result == "UNAVAILABLE" else 409
            payload = {"result": exc.result, "error": str(exc),
                       "receiver_execution": "UNVERIFIED"}
            if (is_source or is_cd or is_volume or is_playback or is_power or
                    is_monitor or is_query) and exc.evidence and not (
                        stop_cd and self.source_stage == "CD_STOP"):
                payload.update(exc.evidence)
            if is_power or is_monitor:
                if is_power:
                    payload["action"] = action
                # Never retried; only a clean refusal certainly sent nothing.
                payload["physical_check_required"] = (
                    not isinstance(exc, PowerToggleRefused) and
                    exc.result not in ("UNAVAILABLE", "REJECTED"))
            if stop_cd:
                payload["source"] = source
                payload["stage"] = self.source_stage
                payload["cd_stop_sent"] = self.source_stage == "CD_STOP"
                if self.source_stage == "CD_STOP":
                    # The source was verified; the failed STOP's evidence is its own.
                    payload.update(self.source_evidence)
                    payload["cd_stop"] = dict(exc.evidence or {}, result=exc.result)
                    # The receiver has switched and the changer may still play.
                    payload["physical_check_required"] = True
                else:
                    # CD_STOP was never sent; only a clean refusal sent nothing.
                    payload["physical_check_required"] = exc.result not in (
                        "UNAVAILABLE", "REJECTED")
            if is_volume:
                payload["direction"] = direction
                # Never retried here or in the browser; only a clean refusal sent nothing.
                payload["physical_check_required"] = exc.result not in (
                    "UNAVAILABLE", "REJECTED")
            if is_cd or is_playback:
                payload["action"] = action
                if is_select:
                    payload["track"] = track
                # Anything but a clean refusal may have moved the changer.
                payload["physical_check_required"] = exc.result not in (
                    "UNAVAILABLE", "REJECTED")
            self.send_json(code, payload)
            return
        payload = {"result": "SERVICE_TX_OK", "receiver_execution": "UNVERIFIED"}
        if is_source or is_monitor:
            # Still under every lock this request holds; a failed SYNC never
            # repeats the control and leaves the last observation in place.
            payload["sync"] = (self.dispatch_sync(session, requested=source) if is_source
                               else self.dispatch_sync(session,
                                                       expected_monitor=expected_monitor))
        if is_volume:
            payload["direction"] = direction
            payload.update(reply)
            # Same single-use successor as CD track; GET /api/v1/session grants it too.
            payload["csrf_token"] = session["csrf"]
        elif is_cd or is_playback:
            payload["action"] = action
            if is_select:
                payload["track"] = track
            if action in CD_PROVISIONAL_ACTIONS:
                payload["mapping"] = "PROVISIONAL"
            payload.update(reply)
            # The token rotated before dispatch, so the one just used is already
            # invalid. Returning its single-use successor (only on a verified
            # pair, only in this same-origin, no-store body) lets the browser send
            # an explicit second PREV without three refresh round trips. It
            # grants nothing GET /api/v1/session would not.
            payload["csrf_token"] = session["csrf"]
        elif is_power:
            payload["action"] = action
            payload.update(reply)
            if action in POWER_PROVISIONAL_ACTIONS:
                payload["mapping"] = "PROVISIONAL"
        elif is_monitor:
            # The indicator changes only from observed receiver reports.
            payload.update(reply)
            payload["mapping"] = "PROVISIONAL"
        elif is_query:
            payload.update(reply)
        elif is_mute:
            payload["service_reply"] = reply
        else:
            payload.update(reply)
            if stop_cd:
                payload["source"] = source
                payload["cd_stop"] = self.cd_stop_evidence
        self.send_json(200, payload)


    @staticmethod
    def parse_seek(body, kind="cd"):
        try:
            pairs = json.loads(body, object_pairs_hook=list)
        except (UnicodeError, ValueError):
            return None
        if (not isinstance(pairs, list) or
                not all(isinstance(pair, tuple) and isinstance(pair[1], str) for pair in pairs)):
            return None
        request = dict(pairs)
        if len(request) != len(pairs):
            return None
        action = request.get("action")
        hold_id = request.get("hold_id")
        if hold_id is not None and not (0 < len(hold_id) <= 43 and all(
                c.isascii() and (c.isalnum() or c in "-_") for c in hold_id)):
            return None
        if action == "START" and set(request) == {"action", "direction"} and \
                request["direction"] in KEY_HOLDS[kind]["directions"]:
            return request
        if action == "HEARTBEAT" and set(request) == {"action", "hold_id"}:
            return request
        if action == "RELEASE" and set(request) in ({"action"}, {"action", "hold_id"}):
            return request
        return None

    def seek_failure(self, exc, physical):
        code = {"UNAVAILABLE": 503, "REJECTED": 409, "CD_FAULT_LATCHED": 409,
                "NOT_ACTIVE": 409, "TIMEOUT": 504}.get(exc.result, 502)
        payload = {"result": exc.result, "error": str(exc),
                   "physical_check_required": physical}
        if exc.evidence:
            payload.update(exc.evidence)
        return code, payload

    def handle_seek(self, session, request, kind="cd"):
        """START, HEARTBEAT or RELEASE of the one server-wide held CD seek.

        The hold is owned by one persistent service connection: KEY_DOWN starts
        it, repeated KEY_DOWN on that connection keeps it alive without a new
        XS8 frame, KEY_UP ends it, and closing the connection makes the service
        release at once. Nothing is ever reconnected or resent."""
        # Single use, like every control POST. Every reply that passed the CSRF
        # check carries the successor so RELEASE never waits for a token refresh;
        # it grants nothing GET /api/v1/session would not.
        session["csrf"] = secrets.token_urlsafe(32)

        def reply(code, payload):
            payload["receiver_execution"] = "UNVERIFIED"
            payload["csrf_token"] = session["csrf"]
            self.send_json(code, payload)

        registry = self.cd_seek_state()
        hold = registry["hold"]
        # Another kind's hold (CD seek versus volume) is not this endpoint's.
        if hold is not None and hold["kind"] != kind:
            hold = None
        config = KEY_HOLDS[kind]
        action = request["action"]
        now = time.monotonic()
        if action == "HEARTBEAT":
            if (hold is None or hold["session"] is not session or
                    not secrets.compare_digest(hold["hold_id"], request["hold_id"])):
                reply(409, {"result": "NOT_ACTIVE"})
                return
            if (now - hold["started"]) * 1000 >= hold["max_ms"]:
                # Independent backstop: never renew past the maximum; closing
                # the owning connection makes the service release now.
                end_cd_seek(self.server, "MAX_HOLD")
                reply(409, {"result": "NOT_ACTIVE", "reason": "MAX_HOLD"})
                return
            started = time.monotonic()
            try:
                line = hold["connection"].exchange(f"KEY_DOWN {hold['action']}", 1.0)
            except MuteServiceError as exc:
                # The connection is closed, so the service is releasing. Whether
                # this renewal arrived first is unknown.
                end_cd_seek(self.server, exc.result)
                code, payload = self.seek_failure(exc, True)
                reply(code, payload)
                return
            finally:
                self.add_timing("xs8heartbeat", started)
            if line == f"OK KEY_ALIVE action={hold['action']}":
                hold["browser_seen"] = time.monotonic()
                reply(200, {"result": "LEASE_RENEWED"})
                return
            # Never a new press: the service refuses a late keepalive with
            # KEY_EXPIRED after INACTIVITY or MAX_HOLD; any other reply is unexpected.
            end_cd_seek(self.server, "HEARTBEAT_REFUSED")
            if line.startswith("ERR KEY_EXPIRED last=") and len(line) > 21:
                reply(409, {"result": "NOT_ACTIVE", "cd_last_result": line[21:]})
            else:
                code, payload = self.seek_failure(
                    MuteServiceError("INVALID_REPLY", "Unexpected XS8 keepalive reply"), True)
                reply(code, payload)
            return
        if action == "RELEASE":
            if hold is None or hold["session"] is not session:
                reply(200, {"result": "NOT_ACTIVE"})
                return
            if "hold_id" in request and not secrets.compare_digest(
                    hold["hold_id"], request["hold_id"]):
                # A stale release never stops a newer hold.
                reply(409, {"result": "NOT_OWNER"})
                return
            started = time.monotonic()
            try:
                result, evidence = parse_key_up(
                    hold["connection"].exchange(f"KEY_UP {hold['action']}", 2.0),
                    hold["action"])
            except MuteServiceError as exc:
                # SERVICE_UNCERTAIN: the service latched a CD fault and kept its
                # obligation. Otherwise delivery is unknown; closing the
                # connection below still makes the service release.
                code, payload = self.seek_failure(exc, True)
                reply(code, payload)
                return
            finally:
                # Always: the connection's only job was this hold.
                end_cd_seek(self.server, "RELEASE")
                (self.cd_track_state() if kind == "cd" else
                 self.volume_step_state())["completed"] = time.monotonic()
                self.add_timing("xs8hold", started)
            reply(200, {"result": result, **evidence})
            return
        direction = request["direction"]
        key_action = config["directions"][direction]
        if kind == "cd":
            state, interval = self.cd_track_state(), self.cd_track_interval_ms
            busy = "CD_BUSY"
        else:
            state, interval = self.volume_step_state(), VOLUME_STEP_INTERVAL_MS
            busy = "VOLUME_BUSY"
        if not state["lock"].acquire(blocking=False):
            reply(409, {"result": busy})
            return
        self.held_lock = state["lock"]
        if (interval != CD_TRACK_INTERVAL_OFF and
                (now - state["completed"]) * 1000 < interval):
            reply(429, {"result": "TOO_FAST", "min_interval_ms": interval})
            return
        try:
            status = self.read_service_status()
        except ServiceError:
            reply(503, {"result": "UNAVAILABLE"})
            return
        self.forget_stale_hold(status)
        if self.cd_fault(status):
            reply(409, {"result": "CD_FAULT_LATCHED", "physical_check_required": True})
            return
        if kind == "volume" and status.get("state") == "FAULT_LATCHED":
            reply(409, {"result": "FAULT_LATCHED", "physical_check_required": True})
            return
        if status.get("cd_state") in ("ACTIVE", "HOLDING"):
            reply(409, {"result": "CD_BUSY"})
            return
        if (status.get("state") in ("HOLDING_UP", "HOLDING_DOWN") or
                registry["hold"] is not None):
            reply(409, {"result": busy})
            return
        if not (self.status_allows_cd_track(status) if kind == "cd"
                else self.status_allows_volume_step(status)):
            reply(409, {"result": "NOT_ELIGIBLE"})
            return
        started = time.monotonic()
        connection = None
        try:
            connection = ServiceConnection(self.socket_path, 3.0)
            evidence = parse_key_down(connection.exchange(f"KEY_DOWN {key_action}", 3.0),
                                      key_action)
        except MuteServiceError as exc:
            # Closing makes the service release any press it did make.
            if connection is not None:
                connection.close()
            code, payload = self.seek_failure(
                exc, exc.result not in ("UNAVAILABLE", "REJECTED"))
            payload["direction"] = direction
            reply(code, payload)
            return
        finally:
            state["completed"] = time.monotonic()
            self.add_timing("xs8cd", started)
        now = time.monotonic()
        hold_id = secrets.token_urlsafe(16)
        registry["hold"] = {"session": session, "connection": connection, "kind": kind,
                            "hold_id": hold_id, "direction": direction,
                            "action": key_action, "started": now, "browser_seen": now,
                            "max_ms": config["max_ms"]}
        try:
            reply(200, {"result": "SEEK_STARTED" if kind == "cd" else "HOLD_STARTED",
                        "direction": direction, "hold_id": hold_id,
                        **evidence, "heartbeat_ms": CD_SEEK_HEARTBEAT_MS,
                        "lease_ms": CD_SEEK_LEASE_MS, "max_hold_ms": config["max_ms"]})
        except OSError:
            # The browser never learns of this hold: release it now.
            end_cd_seek(self.server, "START_REPLY_UNDELIVERED")


class ActivatedUnixServer(CdSeekCleanupMixin, socketserver.TCPServer):
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
    parser.add_argument("--enable-receiver-query", action="store_true",
                        help="enable one explicit authenticated receiver status query")
    parser.add_argument("--enable-cd-track", action="store_true",
                        help="enable discrete CD NEXT/PREVIOUS through the real local XS8 Service")
    parser.add_argument("--enable-cd-track-select", action="store_true",
                        help="enable direct CD track entry (tracks 1-39) through the real local "
                             "XS8 Service; independent of --enable-cd-track")
    parser.add_argument("--enable-volume", action="store_true",
                        help="enable single atomic VOLUME_STEP taps through the real local XS8 Service")
    parser.add_argument("--enable-cd-playback", action="store_true",
                        help="enable CD PLAY/PAUSE (provisional 0xBC) and STOP through the real local XS8 Service")
    parser.add_argument("--enable-cd-seek", action="store_true",
                        help="enable held CD FF/REW through the real local XS8 Service hold API")
    parser.add_argument("--enable-volume-hold", action="store_true",
                        help="enable held VOL+/VOL- on a connection-owned service hold")
    parser.add_argument("--enable-power", action="store_true",
                        help="enable POWER ON (ROOT 0x52) and POWER OFF (ROOT 0x62)")
    parser.add_argument("--enable-tape2-monitor", action="store_true",
                        help="enable the unvalidated TAPE2 MONITOR toggle (CONTEXT 0x48)")
    parser.add_argument("--cd-track-interval-ms", type=int,
                        help="experimental minimum ms from one completed CD track "
                             f"command to the next (default {CD_TRACK_INTERVAL_MS}, "
                             f"range {CD_TRACK_INTERVAL_RANGE_MS[0]}-"
                             f"{CD_TRACK_INTERVAL_RANGE_MS[1]}, or "
                             f"{CD_TRACK_INTERVAL_OFF} for experimental zero cooldown)")
    parser.add_argument("--https-origin", help="required exact HTTPS Origin for active controls")
    parser.add_argument("--systemd-socket", action="store_true",
                        help="serve read-only HTTP using the systemd Unix listener")
    parser.add_argument("--nginx-uid", type=int,
                        help="expected nginx worker UID for --systemd-socket")
    args = parser.parse_args(argv)
    if args.enable_dev_mute:
        parser.error("MUTE development gate is disabled in phase 2A")
    if args.cd_track_interval_ms is not None:
        low, high = CD_TRACK_INTERVAL_RANGE_MS
        if not args.enable_cd_track:
            parser.error("--cd-track-interval-ms requires --enable-cd-track")
        if (args.cd_track_interval_ms != CD_TRACK_INTERVAL_OFF and
                not low <= args.cd_track_interval_ms <= high):
            parser.error(f"--cd-track-interval-ms must be {CD_TRACK_INTERVAL_OFF} "
                         f"or {low}-{high}")
    else:
        args.cd_track_interval_ms = CD_TRACK_INTERVAL_MS
    if args.enable_offline_mute and (args.enable_mute or args.enable_source or
                                    args.enable_receiver_query or args.enable_cd_track or
                                    args.enable_volume or args.enable_cd_playback or
                                    args.enable_cd_seek or args.enable_volume_hold or
                                    args.enable_power or args.enable_tape2_monitor or
                                    args.enable_cd_track_select):
        parser.error("offline and production MUTE modes are mutually exclusive")
    if (args.enable_offline_mute or args.enable_mute or args.enable_source or
            args.enable_receiver_query or args.enable_cd_track or args.enable_volume or
            args.enable_cd_playback or args.enable_cd_seek or args.enable_volume_hold or
            args.enable_power or args.enable_tape2_monitor or args.enable_cd_track_select):
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
                    "receiver_query_enabled": args.enable_receiver_query,
                    "cd_track_enabled": args.enable_cd_track,
                    "cd_track_interval_ms": args.cd_track_interval_ms,
                    "volume_step_enabled": args.enable_volume,
                    "cd_playback_enabled": args.enable_cd_playback,
                    "cd_seek_enabled": args.enable_cd_seek,
                    "volume_hold_enabled": args.enable_volume_hold,
                    "power_enabled": args.enable_power,
                    "tape2_monitor_enabled": args.enable_tape2_monitor,
                    "cd_track_select_enabled": args.enable_cd_track_select,
                    "https_origin": args.https_origin, "sessions": {}})
    if args.systemd_socket:
        try:
            server = ActivatedUnixServer(systemd_listener(), handler, args.nginx_uid)
        except (OSError, ValueError) as exc:
            print(f"XS8 gateway socket activation failed: {exc}", file=sys.stderr)
            return 1
        mode = ("OFFLINE FAKE MUTE TEST" if args.enable_offline_mute else
                " + ".join(name for name, enabled in (
                    ("PRODUCTION MUTE", args.enable_mute),
                    ("PRODUCTION SOURCE", args.enable_source),
                    ("RECEIVER QUERY", args.enable_receiver_query),
                    (f"CD TRACK {args.cd_track_interval_ms} ms", args.enable_cd_track),
                    ("VOLUME STEP", args.enable_volume),
                    ("CD PLAYBACK", args.enable_cd_playback),
                    ("CD SEEK", args.enable_cd_seek),
                    ("VOLUME HOLD", args.enable_volume_hold),
                    ("POWER", args.enable_power),
                    ("TAPE2 MONITOR", args.enable_tape2_monitor),
                    ("CD TRACK SELECT", args.enable_cd_track_select))
                    if enabled) or "read-only")
        print(f"XS8 gateway listening on {GATEWAY_SOCKET} ({mode})")
    else:
        server = GatewayHTTPServer((args.bind, args.port), handler)
        print(f"XS8 gateway listening on http://{args.bind}:{server.server_port} (read-only)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
