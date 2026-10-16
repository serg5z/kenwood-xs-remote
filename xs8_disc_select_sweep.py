#!/usr/bin/env python3
"""Operator-attended DISC SELECT prefix search: one ROOT chain PREFIX -> SUFFIX per trial.

Every trial is one RESEARCH_CHAIN on the XS8 Service. The harness never
retries, never sends a recovery byte, and never moves on to another candidate
set. It stops at the first D4 (POSSIBLE_DISC_CHANGE), unexpected RX, TX or
monitor fault, abnormal STATUS, or any operator observation other than the
expected track change. RX silence is recorded as RX_SILENT: the bus cannot
show a track change, so only the operator's checkpoint decides the outcome.

--unattended-rx-only drops every operator checkpoint and track reset: each
pair is sent after a fixed delay while TX, STATUS and RX stay clean. Without
resets it is an anomaly-discovery sweep only. Its RX_SILENT trials are never
negatives, and a completed run does not rule out DISC SELECT.

--observe-delay (unattended only) spends each inter-trial delay, and the same
time after the last trial, in back-to-back passive RESEARCH_OBSERVE windows.
Any frame there stops the run, so a late report such as the DE -> 7D seen
about 6 s after DISC SKIP is not lost between trials. Observation sends nothing.

--grouped N sends the plan in consecutive groups of N prefixes (unattended
style inside a group). Before each group it shows the group's prefixes and
waits for the operator to restore the changer by hand and type
"start-group N"; after each group it shows what was sent and waits for "ok"
or "anomaly". ANOMALY stops with the changer left as it is. The harness sends
no probe or reset. This is a group-level check: no pair is physically
verified.

--correlate-journal checks a finished log against an exported service
journal: every OBSERVED_BUS frame in the run must be an echo of a logged TX.

--dry-run talks to an in-process mock service and never opens the socket.
"""

import argparse
import datetime
import hashlib
import json
import signal
import sys
import time
from pathlib import Path

import xs8_repl


# The ROOT no-visible-effect union (research/notes/xs8-key-release-candidates.md).
# A candidate file cannot widen it: known, dangerous, context-active and
# ambiguous bytes are refused here whatever the data says.
ALLOWED_PREFIXES = frozenset(int(b, 16) for b in """
00 01 02 03 06 0A 0B 0C 0D 0E 0F 10 11 12 16 19 1A 1B 1C 1D 1E 1F 20 22 23 26 27
29 2A 2B 2C 2D 2E 2F 30 32 33 37 3A 3B 3C 3D 3E 3F 40 43 47 4A 4B 4C 4D 4E 4F 50
53 57 59 5A 5B 5C 5D 5E 5F 60 63 6B 6C 6D 6E 6F 70 73 79 7B 7C 7D 7E 7F 80 87 89
8B 8C 8D 8E 8F 90 91 92 93 95 96 97 99 9A 9B 9D 9E 9F A0 A2 A3 A4 A7 A8 AA AB AC
AD AE AF B0 B1 B2 B3 B5 B7 B8 B9 BA BB BD BE BF C0 C1 C2 C4 C5 C6 C7 C9 CA CB CD
CE CF D0 D1 D2 D4 D7 D9 DB DC DD DE DF E0 E1 E2 E7 E8 EB ED EE EF F0 F2 F3 F5 F6
F7 F8 FA FB FD FE FF""".split())
assert len(ALLOWED_PREFIXES) == 169
ALLOWED_SUFFIXES = frozenset({0x48})  # TRACK 4, physically validated.

MAX_OBSERVE_MS = 5000  # The service's research window limit.
DISC_CHANGE_LEGACY = 0xD4  # Candidate disc-change report; same in both notations.
NOMINAL_STATUS = {
    "state": "IDLE", "stop_owed": "0", "context_may_be_open": "0",
    "monitor": "HEALTHY", "cd_state": "IDLE", "cd_release_owed": "0",
    "cd_lease_expired": "0", "receiver_integrity": "HEALTHY",
}
START_PHRASE = "START DISC SELECT TRIALS"
READ_DEADLINE_S = 10.0

# Operator checkpoint answers. Only EXPECTED continues.
EXPECTED = "t4"
OBSERVATIONS = {
    "t4": "Same disc, now on track 4; nothing else changed (digit read as a track)",
    "disc": "Changer moved to another disc",
    "none": "No change at all (digit not applied)",
    "other": "Any other change: source, indicator, lock, stop/pause, display",
    "abort": "Stop now without classifying",
}
PANEL_EVERY = 5  # Completed trials between receiver front-panel checks, and at the end.
PANEL_ANSWERS = ("panel-ok", "fail", "abort")


class Stop(Exception):
    """Ends the run. Never followed by a retry or recovery transmission."""

    def __init__(self, verdict, detail):
        super().__init__(f"{verdict}: {detail}")
        self.verdict = verdict
        self.detail = detail


def legacy(value):
    """legacy_msb_short1 label of a waveform byte: reverse8(~value)."""
    return int(f"{~value & 0xFF:08b}"[::-1], 2)


def parse_byte(text):
    if not xs8_repl.BYTE_RE.fullmatch(text):
        raise ValueError(f"not a 0xNN byte: {text!r}")
    return int(text, 16)


def make_groups(candidates, size):
    """Consecutive groups of the full plan; numbering never depends on resume."""
    groups = []
    for number, start in enumerate(range(0, len(candidates), size), 1):
        members = candidates[start:start + size]
        groups.append({"number": number, "members": members,
                       "first_rank": members[0]["rank"], "last_rank": members[-1]["rank"]})
    return groups


def load_candidates(path, set_name, unattended=False):
    """Returns (suffix, candidates, skipped). Each skipped item has a skip_reason.

    Tested bytes are always skipped. Bytes listed under reserved_attended are
    also skipped in unattended runs, but stay in attended plans.
    """
    data = json.loads(Path(path).read_text())
    if data.get("schema") != 1:
        raise ValueError("unsupported candidate file schema")
    sets = data.get("sets", {})
    if set_name not in sets:
        raise ValueError(f"unknown set {set_name!r}; available: {', '.join(sorted(sets))}")
    suffix = parse_byte(data["suffix"])
    if suffix not in ALLOWED_SUFFIXES:
        raise ValueError(f"suffix 0x{suffix:02X} is not an allowed suffix")
    tested = {parse_byte(t["byte"]) for t in data.get("tested", [])
              if parse_byte(t.get("suffix", data["suffix"])) == suffix}
    reserved = {parse_byte(r["byte"]) for r in data.get("reserved_attended", [])}
    entries = sets[set_name]["candidates"]
    ranks = [e["rank"] for e in entries]
    if ranks != list(range(1, len(entries) + 1)):
        raise ValueError("ranks must run 1..N in file order")
    seen = set()
    candidates, skipped = [], []
    for entry in entries:
        byte = parse_byte(entry["byte"])
        if byte in seen:
            raise ValueError(f"duplicate candidate 0x{byte:02X}")
        seen.add(byte)
        if byte not in ALLOWED_PREFIXES:
            raise ValueError(f"0x{byte:02X} is outside the allowed prefix union")
        item = dict(entry, value=byte)
        if byte in tested:
            skipped.append(dict(item, skip_reason="already tested"))
        elif unattended and byte in reserved:
            skipped.append(dict(item, skip_reason="reserved for an attended experiment"))
        else:
            candidates.append(item)
    return suffix, candidates, skipped


UNATTENDED = "unattended-rx-only"
GROUPED = "grouped-operator"


def approval_digest(set_name, suffix, gap_ms, window_ms, candidates, mode=None, delay_s=None,
                    observe_delay=False, group_size=None):
    """Binds an approval to the exact set, order, suffix and timing.

    The attended digest keeps its original form. The unattended mode adds the
    mode and the inter-trial delay, so no attended approval can authorize it.
    """
    head = [set_name, f"0x{suffix:02X}", str(gap_ms), str(window_ms)]
    if mode is not None:
        head += [f"mode={mode}", f"delay_s={delay_s:g}"]
        if observe_delay:
            head.append("observe_delay=1")
        if group_size is not None:
            head.append(f"group_size={group_size}")
    text = "|".join(head + [f"{c['rank']}:0x{c['value']:02X}" for c in candidates])
    return hashlib.sha256(text.encode()).hexdigest()[:16]


def completed_bytes(log_path, dry_run, mode="attended"):
    """Candidates already attempted in an earlier log: never sent again.

    A log belongs to either dry runs or real runs; mixing them is refused so
    mock trials can never hide a real candidate (or the reverse).
    """
    done = set()
    path = Path(log_path)
    if not path.exists():
        return done
    for line in path.read_text().splitlines():
        record = json.loads(line)
        if record.get("event") == "run" and bool(record.get("dry_run")) != dry_run:
            raise ValueError(f"{log_path} holds {'real' if dry_run else 'dry-run'} "
                             "records; use a separate log")
        if record.get("event") == "run" and record.get("mode", "attended") != mode:
            raise ValueError(f"{log_path} holds {record.get('mode', 'attended')} records; "
                             "use a separate log")
        if record.get("event") == "trial" and record.get("chain_sent"):
            done.add(parse_byte(record["prefix"]))
    return done


def status_problems(status_line):
    if not status_line.startswith("OK "):
        return [f"STATUS reply {status_line!r}"]
    fields = xs8_repl.fields(status_line)
    problems = [f"{key}={fields.get(key, '<missing>')}"
                for key, want in NOMINAL_STATUS.items() if fields.get(key) != want]
    if fields.get("receiver_source") not in ("CD", None):
        problems.append(f"receiver_source={fields['receiver_source']}")
    return problems


class SocketService:
    def __init__(self, path):
        self.path = path

    def request(self, command):
        return xs8_repl.request(self.path, command)


class MockService:
    """In-process stand-in for xs8-service research chains. Transmits nothing.

    scenario maps a prefix byte to overrides: rx (list of (offset_ms, legacy
    byte) after the suffix), tx_fail (frame index), pre_status / post_status
    (STATUS lines), monitor_failed (bool), echo_missing (bool), done_aborted
    (bool: TX_DONE reports an abort although each frame record is clean).
    """

    IDLE = ("OK state=IDLE stop_owed=0 context_may_be_open=0 monitor=HEALTHY "
            "last_byte=0x13 last_gpio_tx=TX_OK last_waveform=MATCH cd_state=IDLE "
            "cd_action=STOP cd_release_owed=0 cd_lease_expired=0 receiver_source=CD "
            "receiver_integrity=HEALTHY")

    def __init__(self, scenario=None):
        self.scenario = scenario or {}
        self.commands = []
        self.chains = {}
        self.next_id = 100
        self.observed = []
        self.clock_ns = 62_913_000_000_000
        self.current = None

    def _status(self):
        if self.current is None or self.current[0] is None:
            return self.IDLE
        prefix, phase = self.current
        key = "pre_status" if phase == "pre" else "post_status"
        return self.scenario.get(prefix, {}).get(key, self.IDLE)

    def request(self, command):
        self.commands.append(command)
        parts = command.split()
        if command == "STATUS":
            return self._status()
        if parts[0] == "RESEARCH_CHAIN":
            return self._chain(parts)
        if parts[0] == "RESEARCH_TX_READ":
            tx = self.chains[int(parts[1])]["tx"]
            index = int(parts[2])
            return tx[index] if index < len(tx) else self.chains[int(parts[1])]["tx_done"]
        if parts[0] == "RESEARCH_READ":
            rx = self.chains[int(parts[1])]["rx"]
            index = int(parts[2])
            return rx[index] if index < len(rx) else self.chains[int(parts[1])]["done"]
        if parts[0] == "RESEARCH_OBSERVE":
            return self._observe(int(parts[1]))
        if parts[0] == "RESEARCH_CANCEL":
            return f"OK RESEARCH_CANCELLED id={parts[1]}"
        return "ERR UNKNOWN_COMMAND"

    def _observe(self, window_ms):
        """Passive window. scenario[prefix]["between_rx"] frames land in the first one."""
        prefix = self.current[0] if self.current else None
        scenario = self.scenario.get(prefix, {})
        frames = scenario.pop("between_rx", []) if scenario.get("between_rx") else []
        observe_id = self.next_id
        self.next_id += 1
        start = self.clock_ns + 2_000_000
        deadline = start + window_ms * 1_000_000
        records = [
            f"OK RESEARCH_RECORD id={observe_id} index={i} kind=FRAME "
            f"start_ns={start + offset * 1_000_000} end_ns={start + offset * 1_000_000 + 100_000_000} "
            f"waveform_lsb=0x{legacy(label):02X} legacy_msb_short1=0x{label:02X} valid=1 bits=8 "
            "lost_events=0 reason=NONE relation=OTHER_TRAFFIC origin=UNATTRIBUTED "
            "receiver_execution=UNVERIFIED"
            for i, (offset, label) in enumerate(frames)]
        failed = int(bool(scenario.get("observe_monitor_failed")))
        self.chains[observe_id] = {
            "tx": [], "tx_done": "", "rx": records,
            "done": (f"OK RESEARCH_DONE id={observe_id} count={len(records)} start_ns={start} "
                     f"deadline_ns={deadline} kind=OBSERVE overflow=0 "
                     f"monitor_failed={failed} cancelled=0")}
        self.clock_ns = deadline + 3_000_000
        self.observed.append(window_ms)
        return f"OK RESEARCH_OBSERVE id={observe_id} start_ns={start} deadline_ns={deadline}"

    def prepare(self, prefix):
        self.current = (prefix, "pre")

    def _chain(self, parts):
        count, gap_ms, window_ms = int(parts[1]), int(parts[2]), int(parts[3])
        values = [int(b, 16) for b in parts[4:]]
        prefix = values[0]
        scenario = self.scenario.get(prefix, {})
        chain_id = self.next_id
        self.next_id += 1
        tx, rx, start = [], [], self.clock_ns
        t = start + 3_000_000
        last_end = t
        attempted = 0
        for index, value in enumerate(values):
            failed = scenario.get("tx_fail") == index
            end = t + 100_000_000
            tx.append(f"OK RESEARCH_TX_RECORD id={chain_id} index={index} "
                      f"requested=0x{value:02X} tx_start_ns={t} tx_end_ns={end} "
                      f"gpio_tx={'TX_FAILED' if failed else 'TX_OK'} "
                      f"waveform={'UNCERTAIN' if failed else 'MATCH'} "
                      "receiver_execution=UNVERIFIED")
            attempted += 1
            if not scenario.get("echo_missing"):
                rx.append((t + 30_000, end - 400_000, value, legacy(value), "TX_WINDOW"))
            if failed:
                break
            last_end = end
            t = end + gap_ms * 1_000_000 + 14_000_000
        aborted = int(attempted < count or bool(scenario.get("done_aborted")))
        for offset_ms, label in scenario.get("rx", []):
            begin = last_end + offset_ms * 1_000_000
            waveform = legacy(label)  # The conversion is its own inverse.
            rx.append((begin, begin + 100_000_000, waveform, label, "OTHER_TRAFFIC"))
        records = [
            f"OK RESEARCH_RECORD id={chain_id} index={i} kind=FRAME start_ns={b} "
            f"end_ns={e} waveform_lsb=0x{w:02X} legacy_msb_short1=0x{l:02X} valid=1 "
            f"bits=8 lost_events=0 reason=NONE relation={rel} origin=UNATTRIBUTED "
            "receiver_execution=UNVERIFIED"
            for i, (b, e, w, l, rel) in enumerate(rx)]
        self.chains[chain_id] = {
            "tx": tx,
            "tx_done": (f"OK RESEARCH_TX_DONE id={chain_id} count={attempted} "
                        f"planned={count} aborted={aborted} "
                        f"reason={'TX_UNCERTAIN' if aborted else 'NONE'}"),
            "rx": records,
            "done": (f"OK RESEARCH_DONE id={chain_id} count={len(records)} "
                     f"start_ns={start} deadline_ns={t + window_ms * 1_000_000} "
                     f"kind=CHAIN overflow=0 "
                     f"monitor_failed={int(bool(scenario.get('monitor_failed')))} "
                     "cancelled=0"),
        }
        self.clock_ns = t + 10_000_000_000
        self.current = (prefix, "post")
        return (f"OK RESEARCH_CHAIN id={chain_id} count={count} gap_ms={gap_ms} "
                f"window_ms={window_ms} context=ROOT_ASSUMED")


class Sweep:
    def __init__(self, service, suffix, candidates, *, gap_ms, window_ms, delay_s,
                 log, output=print, ask=input, sleep=time.sleep, attended=True,
                 observe_delay=False, groups=None):
        self.service = service
        self.suffix = suffix
        self.candidates = candidates
        self.gap_ms = gap_ms
        self.window_ms = window_ms
        self.delay_s = delay_s
        self.log = log
        self.output = output
        self.ask = ask
        self.sleep = sleep
        self.interrupted = False
        self.attended = attended
        self.observe_delay = observe_delay
        self.covered_until_ns = None
        self.groups = groups
        self.group = None
        self.group_plan = []

    def _read(self, command, deadline):
        """Indexed read-only requests may be repeated after ERR BUSY; nothing else is."""
        while True:
            try:
                reply = self.service.request(command)
                if reply != "ERR BUSY":
                    return reply
            except OSError as error:
                if time.monotonic() >= deadline:
                    raise Stop("SERVICE_UNREACHABLE", f"{command}: {error}")
            if time.monotonic() >= deadline:
                raise Stop("SERVICE_BUSY", command)
            self.sleep(0.05)

    def _status(self, when, record):
        line = self.service.request("STATUS")
        record[f"{when}_status"] = line
        problems = status_problems(line)
        if problems:
            raise Stop("ANOMALOUS_STATUS", f"{when}-trial " + ", ".join(problems))
        return line

    def checkpoint(self, prompt, allowed):
        """Blocks until a listed answer. Closed input is a skipped checkpoint: stop."""
        while True:
            try:
                answer = self.ask(prompt).strip()
            except EOFError:
                raise Stop("CHECKPOINT_SKIPPED", "operator input closed at: " + prompt.strip())
            word = answer.split(" ", 1)[0].lower() if answer else ""
            if word in allowed:
                return word, answer
            self.output(f"Answer one of: {', '.join(allowed)}")

    def run(self):
        self.current = None
        try:
            return self._run()
        except Stop as stop:
            if self.groups is not None:
                self._stop_status()
            return self._finish(stop.verdict, stop.detail, self.current)

    def _stop_status(self):
        """Grouped runs leave the state for manual investigation: record it, change nothing."""
        try:
            line = self.service.request("STATUS")
        except (OSError, ValueError) as error:
            line = f"unavailable: {error}"
        self.log.write({"event": "stop_status", "status": line, "at": now()})

    def panel_check(self, completed):
        word, answer = self.checkpoint(
            f"Receiver front-panel check after {completed} completed trial(s): press a "
            "receiver front-panel key with a visible, reversible effect and undo it. "
            "No XS8 command is sent. Type 'panel-ok <note>', 'fail <note>' or 'abort': ",
            PANEL_ANSWERS)
        self.log.write({"event": "checkpoint", "phase": "panel", "completed": completed,
                        "answer": answer})
        if word == "fail":
            raise Stop("PANEL_CHECK_FAILED", answer)
        if word == "abort":
            raise Stop("OPERATOR_ABORT", "at receiver panel check")

    def _wait(self, candidate):
        self.output(f"Next trial in {self.delay_s:g} s (Ctrl-C or SIGTERM stops)")
        resume_at = time.monotonic() + self.delay_s
        while not self.interrupted and time.monotonic() < resume_at:
            self.sleep(min(0.1, max(0.0, resume_at - time.monotonic())))
        if self.interrupted:
            raise Stop("OPERATOR_ABORT", "interrupt during delay")

    def _observe_quiet(self, candidate, final):
        """Cover the delay with passive windows; any frame at all stops the run."""
        label = "after the last trial" if final else "before the next trial"
        self.output(f"Passive observation {self.delay_s:g} s {label} (Ctrl-C or SIGTERM stops)")
        remaining = int(round(self.delay_s * 1000))
        while remaining > 0:
            if self.interrupted:
                raise Stop("OPERATOR_ABORT", "interrupt during passive observation")
            window = min(MAX_OBSERVE_MS, remaining)
            record = {"event": "observe", "after": f"0x{candidate['value']:02X}",
                      "window_ms": window, "final": final}
            if self.group is not None:
                record["group"] = self.group
            try:
                reply = self.service.request(f"RESEARCH_OBSERVE {window}")
                record["reply"] = reply
                fields = xs8_repl.fields(reply)
                if not reply.startswith("OK RESEARCH_OBSERVE ") or \
                        not fields.get("id", "").isdecimal():
                    raise Stop("OBSERVE_REFUSED", reply)
                start = int(fields["start_ns"])
                if self.covered_until_ns is not None:
                    record["uncovered_ms"] = round((start - self.covered_until_ns) / 1e6, 1)
                deadline = time.monotonic() + READ_DEADLINE_S + window / 1000
                record["rx"], record["done"] = self._rx_records(fields["id"], deadline)
                done = xs8_repl.fields(record["done"])
                self.covered_until_ns = int(done.get("deadline_ns", start))
                if any(done.get(k) != "0" for k in ("overflow", "monitor_failed", "cancelled")):
                    raise Stop("MONITOR_FAULT", record["done"])
                for line in record["rx"]:
                    f = xs8_repl.fields(line)
                    if (f.get("kind") != "FRAME" or f.get("valid") != "1" or
                            f.get("lost_events") != "0"):
                        raise Stop("MONITOR_FAULT", line)
                    offset = round((int(f["start_ns"]) - start) / 1e6, 1)
                    where = f"{offset} ms into a passive window {label}, after 0x{candidate['value']:02X}"
                    if int(f["legacy_msb_short1"], 16) == DISC_CHANGE_LEGACY:
                        raise Stop("POSSIBLE_DISC_CHANGE",
                                   f"RX D4 (candidate disc-change report, not confirmed "
                                   f"success) {where}: {line}")
                    raise Stop("UNEXPECTED_RX", f"{where}: {line}")
                record["verdict"] = "QUIET"
            except Stop as stop:
                record["verdict"] = stop.verdict
                raise
            except (OSError, ValueError) as error:
                record["verdict"] = "SERVICE_UNREACHABLE"
                raise Stop("SERVICE_UNREACHABLE", str(error)) from error
            finally:
                self.log.write(record)
            remaining -= window
        if self.interrupted:
            raise Stop("OPERATOR_ABORT", "interrupt during passive observation")

    def _run_grouped(self):
        """Operator prompts before and after every group; the harness sends only the pairs."""
        pending = [g for g in self.groups if g["plan"]]
        self.output(f"GROUPED: {len(pending)} group(s) to send, prefix -> 0x{self.suffix:02X}, "
                    f"gap {self.gap_ms} ms, window {self.window_ms} ms, delay {self.delay_s:g} s"
                    + (" with passive observation" if self.observe_delay else "")
                    + ". The operator checks the changer before and after every group.")
        for group in self.groups:
            if not group["plan"]:
                self.log.write({"event": "group_skipped", "group": group["number"],
                                "reason": "every prefix already sent in this log"})
        for group in pending:
            self._before_group(group)
            self.group = group["number"]
            self.group_plan = group["plan"]
            self.log.write(group_record("group_start", group))
            for position, candidate in enumerate(group["plan"]):
                self.current = candidate
                if self.interrupted:
                    raise Stop("OPERATOR_ABORT", "interrupt before trial")
                self.trial(candidate)
                if self.interrupted:
                    raise Stop("OPERATOR_ABORT", "interrupt during trial")
                last = position == len(group["plan"]) - 1
                if self.observe_delay:
                    self._observe_quiet(candidate, final=last)
                elif not last:
                    self._wait(candidate)
            self.log.write(group_record("group_end", group))
            self.current = None
            self._after_group(group)
            self.group = None
        return self._finish(
            "SET_COMPLETE_GROUPED",
            f"all {sum(len(g['plan']) for g in pending)} pairs sent in {len(pending)} group(s) "
            "and the operator reported every group OK. Group-level check only: no per-prefix "
            "physical verification, no prefix is a verified negative, and DISC SELECT is not "
            "ruled out")

    def _operator(self, phase, group, prompt, allowed):
        word, answer = self.checkpoint(prompt, allowed)
        self.log.write({"event": "checkpoint", "phase": phase, "group": group["number"],
                        "answer": answer, "at": now()})
        if word == "abort":
            raise Stop("OPERATOR_ABORT", f"{phase} group {group['number']}")
        return word, answer

    def _before_group(self, group):
        """Show the group, then wait for the operator's manual baseline and explicit start."""
        self.output(f"NEXT: group {group['number']}, ranks {group['first_rank']}-"
                    f"{group['last_rank']}, {len(group['plan'])} prefix(es) in order: "
                    + " ".join(f"0x{c['value']:02X}" for c in group["plan"]))
        while True:
            _, answer = self._operator(
                "before", group, f"Restore the changer's known initial state by hand. Nothing is "
                f"sent until you type 'start-group {group['number']}' (or 'abort'): ",
                ("start-group", "abort"))
            if answer.split()[1:] == [str(group["number"])]:
                return
            self.output(f"Type exactly: start-group {group['number']}")

    def _after_group(self, group):
        """Show what was just sent; ANOMALY stops with the changer left as it is."""
        self.output(f"DONE: group {group['number']}, ranks {group['first_rank']}-"
                    f"{group['last_rank']}, sent in order: "
                    + " ".join(f"0x{c['value']:02X}" for c in group["plan"]))
        word, answer = self._operator(
            "after", group, "Check the changer state and digit/track selection by hand. Type "
            "'ok <note>', 'anomaly <description>' or 'abort': ", ("ok", "anomaly", "abort"))
        if word == "anomaly":
            raise Stop("OPERATOR_ANOMALY",
                       f"group {group['number']} (ranks {group['first_rank']}-"
                       f"{group['last_rank']}: " + " ".join(f"0x{c['value']:02X}"
                                                             for c in group["plan"])
                       + f"): {answer}. Changer state left for REPL investigation")

    def _run_unattended(self):
        """No checkpoints or resets: continue only while TX, STATUS and RX stay clean."""
        self.output(f"{len(self.candidates)} trial(s), UNATTENDED RX-ONLY: prefix -> "
                    f"0x{self.suffix:02X}, gap {self.gap_ms} ms, window {self.window_ms} ms, "
                    f"delay {self.delay_s:g} s. Anomaly discovery only; RX_SILENT is not "
                    "a negative.")
        for position, candidate in enumerate(self.candidates):
            self.current = candidate
            if self.interrupted:
                raise Stop("OPERATOR_ABORT", "interrupt before trial")
            self.trial(candidate)
            if self.interrupted:
                raise Stop("OPERATOR_ABORT", "interrupt during trial")
            if self.observe_delay:
                self._observe_quiet(candidate, final=position == len(self.candidates) - 1)
            elif position != len(self.candidates) - 1:
                self._wait(candidate)
        self.current = None
        return self._finish(
            "SET_COMPLETE_RX_SILENT",
            f"all {len(self.candidates)} pairs sent with clean TX, STATUS and RX"
            + (" and quiet passive observation between and after trials" if self.observe_delay
               else "") + ". "
            "No track reset or physical observation: this does NOT rule out DISC SELECT "
            "and no candidate is a verified negative; no wider set is started")

    def _run(self):
        if self.groups is not None:
            return self._run_grouped()
        if not self.attended:
            return self._run_unattended()
        completed = 0
        self.output(f"{len(self.candidates)} trial(s): prefix -> 0x{self.suffix:02X}, "
                    f"gap {self.gap_ms} ms, window {self.window_ms} ms")
        word, answer = self.checkpoint(
            "Initial checkpoint. Receiver on CD and responsive; changer playing a "
            "disc with >= 4 tracks, on a track other than 4; note disc/track. "
            "Type 'ready <disc/track note>' or 'abort': ", ("ready", "abort"))
        self.log.write({"event": "checkpoint", "phase": "initial", "answer": answer})
        if word == "abort":
            return self._finish("OPERATOR_ABORT", "before the first trial")
        for position, candidate in enumerate(self.candidates):
            self.current = candidate
            try:
                self.trial(candidate)
            except Stop as stop:
                return self._finish(stop.verdict, stop.detail, candidate)
            if self.interrupted:
                return self._finish("OPERATOR_ABORT", "interrupt during trial", candidate)
            last = position == len(self.candidates) - 1
            prompt = (f"0x{candidate['value']:02X} RX_SILENT. What did the changer do? "
                      f"[{'/'.join(OBSERVATIONS)}] ")
            word, answer = self.checkpoint(prompt, tuple(OBSERVATIONS))
            self.log.write({"event": "checkpoint", "phase": "observation",
                            "prefix": f"0x{candidate['value']:02X}", "answer": answer,
                            "meaning": OBSERVATIONS[word]})
            if word != EXPECTED:
                verdict = "OPERATOR_ABORT" if word == "abort" else "OPERATOR_OBSERVED_" + word.upper()
                return self._finish(verdict, answer, candidate)
            completed += 1
            if completed % PANEL_EVERY == 0 or last:
                self.panel_check(completed)
            if last:
                break
            word, answer = self.checkpoint(
                "Reset by hand on the changer: same disc, a track other than 4, "
                "playing; receiver still on CD and responsive. "
                "Type 'ready <note>' or 'abort': ", ("ready", "abort"))
            self.log.write({"event": "checkpoint", "phase": "reset", "answer": answer})
            if word == "abort":
                return self._finish("OPERATOR_ABORT", "at reset checkpoint", candidate)
            self._wait(candidate)
        return self._finish("SET_COMPLETE", "every candidate was RX_SILENT, observed as a "
                            "track change, and the final panel check passed; no wider set "
                            "is started", None)

    def _finish(self, verdict, detail, candidate=None):
        record = {"event": "stop", "verdict": verdict, "detail": detail, "at": now()}
        if candidate:
            record["prefix"] = f"0x{candidate['value']:02X}"
            record["rank"] = candidate["rank"]
        if self.group is not None:
            record["group"] = self.group
        self.log.write(record)
        self.output(f"STOP {verdict}: {detail}")
        return verdict

    def trial(self, candidate):
        prefix = candidate["value"]
        record = {"event": "trial", "rank": candidate["rank"], "prefix": f"0x{prefix:02X}",
                  "suffix": f"0x{self.suffix:02X}", "tier": candidate.get("tier"),
                  "gap_ms": self.gap_ms, "window_ms": self.window_ms,
                  "chain_sent": False, "started": now()}
        if self.group is not None:
            record["group"] = self.group
        if self.group is not None:
            position = next(i for i, c in enumerate(self.group_plan, 1) if c is candidate)
            self.output(f">>> GROUP {self.group} | TRIAL {position}/{len(self.group_plan)} | "
                        f"RANK {candidate['rank']} | PREFIX 0x{prefix:02X} -> 0x{self.suffix:02X} "
                        f"| {datetime.datetime.now().strftime('%H:%M:%S')}")
        else:
            self.output(f"Trial rank {candidate['rank']}: 0x{prefix:02X} -> 0x{self.suffix:02X}")
        try:
            if hasattr(self.service, "prepare"):
                self.service.prepare(prefix)
            self._status("pre", record)
            command = (f"RESEARCH_CHAIN 2 {self.gap_ms} {self.window_ms} "
                       f"0x{prefix:02X} 0x{self.suffix:02X}")
            record["chain_sent"] = True
            reply = self.service.request(command)
            record["chain_reply"] = reply
            session = xs8_repl.fields(reply).get("id")
            if not reply.startswith("OK RESEARCH_CHAIN ") or not (session or "").isdecimal():
                raise Stop("CHAIN_REFUSED", reply)
            deadline = time.monotonic() + READ_DEADLINE_S + self.window_ms / 1000
            record["tx"], record["tx_done"] = self._tx_records(session, deadline, prefix)
            record["rx"], record["done"] = self._rx_records(session, deadline)
            record["timing"] = timing(record["tx"], record["rx"])
            if self.covered_until_ns is not None and record["tx"]:
                first_tx = int(xs8_repl.fields(record["tx"][0])["tx_start_ns"])
                record["uncovered_before_ms"] = round((first_tx - self.covered_until_ns) / 1e6, 1)
            self.covered_until_ns = int(xs8_repl.fields(record["done"]).get("deadline_ns", 0)) or None
            self._classify(record, prefix)
            self._status("post", record)
            record["verdict"] = "RX_SILENT"
            self.output(f"  RX_SILENT; TX gap {record['timing'].get('gap_ms')} ms. "
                        "RX silence is not a verified negative.")
        except Stop as stop:
            record["verdict"] = stop.verdict
            record["detail"] = stop.detail
            raise
        except (OSError, ValueError) as error:
            record["verdict"] = "SERVICE_UNREACHABLE"
            record["detail"] = str(error)
            raise Stop("SERVICE_UNREACHABLE", str(error)) from error
        finally:
            record["ended"] = now()
            self.log.write(record)

    def _tx_records(self, session, deadline, prefix):
        expected = [prefix, self.suffix]
        records = []
        while True:
            reply = self._read(f"RESEARCH_TX_READ {session} {len(records)}", deadline)
            if reply.startswith("OK RESEARCH_TX_RECORD "):
                f = xs8_repl.fields(reply)
                records.append(reply)
                if (f.get("gpio_tx") != "TX_OK" or f.get("waveform") != "MATCH" or
                        len(records) > 2 or
                        f.get("requested") != f"0x{expected[len(records) - 1]:02X}"):
                    raise Stop("TX_FAILURE", reply)
            elif reply.startswith("OK RESEARCH_TX_DONE "):
                f = xs8_repl.fields(reply)
                if (f.get("count") != "2" or f.get("planned") != "2" or
                        f.get("aborted") != "0" or len(records) != 2):
                    raise Stop("TX_FAILURE", reply)
                return records, reply
            elif reply.startswith("OK RESEARCH_TX_WAIT "):
                if time.monotonic() >= deadline:
                    raise Stop("SERVICE_TIMEOUT", "TX records")
                self.sleep(0.05)
            else:
                raise Stop("TX_FAILURE", reply)

    def _rx_records(self, session, deadline):
        records = []
        while True:
            reply = self._read(f"RESEARCH_READ {session} {len(records)}", deadline)
            if reply.startswith("OK RESEARCH_RECORD "):
                records.append(reply)
            elif reply.startswith("OK RESEARCH_DONE "):
                return records, reply
            elif reply.startswith("OK RESEARCH_WAIT "):
                if time.monotonic() >= deadline:
                    raise Stop("SERVICE_TIMEOUT", "RX records")
                self.sleep(0.05)
            else:
                raise Stop("MONITOR_FAULT", reply)

    def _classify(self, record, prefix):
        done = xs8_repl.fields(record["done"])
        if any(done.get(k) != "0" for k in ("overflow", "monitor_failed", "cancelled")):
            raise Stop("MONITOR_FAULT", record["done"])
        echoes = []
        for line in record["rx"]:
            f = xs8_repl.fields(line)
            if (f.get("kind") != "FRAME" or f.get("valid") != "1" or
                    f.get("lost_events") != "0"):
                raise Stop("MONITOR_FAULT", line)
            label = int(f["legacy_msb_short1"], 16)
            if f.get("relation") == "TX_WINDOW":
                echoes.append(int(f["waveform_lsb"], 16))
                continue
            if label == DISC_CHANGE_LEGACY:
                raise Stop("POSSIBLE_DISC_CHANGE",
                           "RX D4 (candidate disc-change report, not confirmed success): " + line)
            raise Stop("UNEXPECTED_RX", line)
        if echoes != [prefix, self.suffix]:
            raise Stop("UNEXPECTED_RX",
                       "TX-window frames " + " ".join(f"0x{e:02X}" for e in echoes) +
                       f" differ from the sent 0x{prefix:02X} 0x{self.suffix:02X}")


def group_record(event, group):
    return {"event": event, "group": group["number"], "first_rank": group["first_rank"],
            "last_rank": group["last_rank"],
            "prefixes": [f"0x{c['value']:02X}" for c in group["members"]],
            "plan": [f"0x{c['value']:02X}" for c in group["plan"]], "at": now()}


def timing(tx_lines, rx_lines=()):
    """Durations and offsets in ms, relative to the start of the first TX frame."""
    tx = [xs8_repl.fields(line) for line in tx_lines]
    origin = int(tx[0]["tx_start_ns"]) if tx else 0
    result = {"tx_ms": [round((int(t["tx_end_ns"]) - int(t["tx_start_ns"])) / 1e6, 1)
                        for t in tx],
              "tx_start_ms": [round((int(t["tx_start_ns"]) - origin) / 1e6, 1) for t in tx]}
    if len(tx) == 2:
        result["gap_ms"] = round((int(tx[1]["tx_start_ns"]) - int(tx[0]["tx_end_ns"])) / 1e6, 1)
    rx = []
    for line in rx_lines:
        f = xs8_repl.fields(line)
        if "start_ns" in f:
            rx.append({"start_ms": round((int(f["start_ns"]) - origin) / 1e6, 1),
                       "end_ms": round((int(f["end_ns"]) - origin) / 1e6, 1),
                       "waveform": f.get("waveform_lsb"),
                       "legacy": f.get("legacy_msb_short1"),
                       "relation": f.get("relation")})
    result["rx"] = rx
    return result


def correlate(log_records, journal_lines):
    """Match every journal OBSERVED_BUS frame in the run's span to a logged TX echo.

    Spans run from the first logged TX start to the last capture deadline. A
    frame is an echo when it starts inside a logged TX window (1 ms slack for
    the journal's microsecond rounding). Everything else is reported.

    Group spans are kept per session (the records after each "run" record), so
    a group resumed in a later session does not cover the operator time
    between its parts.
    """
    tx_windows, deadlines, group_spans = [], [], {}
    session = 0
    for record in log_records:
        if record.get("event") == "run":
            session += 1
        if "group" in record and record.get("done"):
            d = xs8_repl.fields(record["done"])
            if "start_ns" in d and "deadline_ns" in d:
                key = (session, record["group"])
                low, high = group_spans.get(key, (None, None))
                start, end = int(d["start_ns"]) // 1000, int(d["deadline_ns"]) // 1000
                group_spans[key] = (start if low is None else min(low, start),
                                    end if high is None else max(high, end))
        for line in record.get("tx", []):
            f = xs8_repl.fields(line)
            tx_windows.append((int(f["tx_start_ns"]) // 1000, int(f["tx_end_ns"]) // 1000,
                               f["requested"], record.get("prefix")))
        done = record.get("done")
        if done:
            deadline = xs8_repl.fields(done).get("deadline_ns")
            if deadline:
                deadlines.append(int(deadline) // 1000)
    if not tx_windows:
        return {"frames": 0, "echoes": 0, "non_echo": [], "between_groups": [],
                "journal_tx_missing": [], "unlogged_tx": [], "span_us": None}
    span = (min(w[0] for w in tx_windows) - 1000, max(deadlines + [w[1] for w in tx_windows]))
    frames, journal_tx = [], []
    for line in journal_lines:
        if "mono_us=" not in line:
            continue
        mono = int(line.split("mono_us=", 1)[1].split()[0])
        if not span[0] <= mono <= span[1]:
            continue
        if " OBSERVED_BUS " in line:
            f = xs8_repl.fields(line)
            frames.append((mono, f.get("waveform_lsb"), f.get("legacy_msb_short1"), line.strip()))
        elif " XS8 TX 0x" in line:
            journal_tx.append((mono, line.split(" XS8 TX ", 1)[1].split()[0]))
    echoes, non_echo, between_groups = 0, [], []
    for mono, waveform, label, line in frames:
        match = [w for w in tx_windows if w[0] - 1000 <= mono <= w[1] and w[2] == waveform]
        if match:
            echoes += 1
        else:
            before = [w for w in tx_windows if w[0] <= mono]
            entry = {"mono_us": mono, "waveform": waveform, "legacy": label,
                     "after_prefix": before[-1][3] if before else None,
                     "ms_after_last_tx": round((mono - before[-1][0]) / 1000, 1)
                     if before else None, "line": line}
            groups = [g for (_, g), (low, high) in group_spans.items() if low <= mono <= high]
            if group_spans and not groups:
                between_groups.append(entry)  # Operator checkpoint time: reported only.
            else:
                entry["group"] = groups[0] if groups else None
                non_echo.append(entry)
    logged = {(w[0], w[2]) for w in tx_windows}
    if group_spans:
        inside = lambda mono: any(low <= mono <= high for low, high in group_spans.values())
        between_groups += [{"mono_us": m, "tx": b} for m, b in journal_tx
                           if (m, b) not in logged and not inside(m)]
        journal_tx = [(m, b) for m, b in journal_tx if (m, b) in logged or inside(m)]
    journal_tx_set = set(journal_tx)
    return {"span_us": span, "frames": len(frames), "echoes": echoes, "non_echo": non_echo,
            "between_groups": between_groups,
            "journal_tx_missing": sorted(f"{m}:{b}" for m, b in logged - journal_tx_set),
            "unlogged_tx": sorted(f"{m}:{b}" for m, b in journal_tx_set - logged)}


def now():
    return datetime.datetime.now().astimezone().isoformat(timespec="milliseconds")


class JsonLog:
    def __init__(self, path):
        self.handle = open(path, "a", encoding="utf-8")

    def write(self, record):
        self.handle.write(json.dumps(record, sort_keys=True) + "\n")
        self.handle.flush()

    def close(self):
        self.handle.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--candidates", default="research/disc-select-candidates.json")
    parser.add_argument("--set", dest="set_name")
    parser.add_argument("--gap-ms", type=int, default=200)
    parser.add_argument("--observe-ms", type=int, default=3000)
    parser.add_argument("--delay-s", type=float, default=5.0,
                        help="pause after each reset checkpoint before the next trial")
    parser.add_argument("--max-trials", type=int, default=0, help="0 = whole set")
    parser.add_argument("--log", help="JSON-lines log; candidates already sent in it are skipped")
    parser.add_argument("--socket", default=xs8_repl.DEFAULT_SOCKET)
    parser.add_argument("--list", action="store_true", help="print the plan and digest; send nothing")
    parser.add_argument("--dry-run", action="store_true", help="mock service; never opens the socket")
    parser.add_argument("--approve", help="approval digest printed by --list (real runs only)")
    parser.add_argument("--unattended-rx-only", action="store_true",
                        help="no operator checkpoints or resets; anomaly discovery only")
    parser.add_argument("--observe-delay", action="store_true",
                        help="unattended only: passive observation through every delay")
    parser.add_argument("--grouped", type=int, metavar="N",
                        help="groups of N prefixes, each followed by an operator checkpoint")
    parser.add_argument("--correlate-journal", metavar="JOURNAL",
                        help="offline: check --log against an exported service journal")
    parser.add_argument("--auto-observe", action="store_true",
                        help="dry-run only: answer every checkpoint as the expected track change")
    args = parser.parse_args(argv)
    if args.correlate_journal:
        if not args.log:
            parser.error("--correlate-journal needs --log")
        records = [json.loads(line) for line in Path(args.log).read_text().splitlines()]
        result = correlate(records, Path(args.correlate_journal).read_text().splitlines())
        print(json.dumps({k: v for k, v in result.items()}, indent=1))
        clean = not result["non_echo"] and not result["journal_tx_missing"] \
            and not result["unlogged_tx"]
        print("CORRELATION " + ("CLEAN: every frame is an echo of a logged TX" if clean
                                else "FOUND FRAMES OR TX NOT ACCOUNTED FOR"))
        if result["between_groups"]:
            print(f"NOTE: {len(result['between_groups'])} frame(s) or TX during operator "
                  "checkpoints, outside every group's capture span (listed above)")
        return 0 if clean else 1
    if not args.set_name:
        parser.error("--set is required")
    if args.grouped is not None:
        if args.unattended_rx_only:
            parser.error("--grouped has its own checkpoints; drop --unattended-rx-only")
        if args.grouped < 1:
            parser.error("--grouped needs a group size of at least 1")
        if args.max_trials:
            parser.error("--max-trials does not apply to --grouped")
    if args.observe_delay and not (args.unattended_rx_only or args.grouped):
        parser.error("--observe-delay needs --unattended-rx-only or --grouped")

    if not xs8_repl.MIN_GAP_MS <= args.gap_ms <= xs8_repl.MAX_GAP_MS:
        parser.error("--gap-ms must be 50..1000")
    if not 1 <= args.observe_ms <= xs8_repl.MAX_WINDOW_MS:
        parser.error("--observe-ms must be 1..5000")
    if args.delay_s < 0 or args.max_trials < 0:
        parser.error("--delay-s and --max-trials must not be negative")
    try:
        suffix, candidates, skipped = load_candidates(
            args.candidates, args.set_name,
            unattended=args.unattended_rx_only or args.grouped is not None)
    except (OSError, ValueError, KeyError) as error:
        parser.error(f"candidate file: {error}")
    mode = (GROUPED if args.grouped is not None else
            UNATTENDED if args.unattended_rx_only else "attended")
    if args.grouped is not None:
        digest = approval_digest(args.set_name, suffix, args.gap_ms, args.observe_ms,
                                 candidates, mode=GROUPED, delay_s=args.delay_s,
                                 observe_delay=args.observe_delay, group_size=args.grouped)
    elif args.unattended_rx_only:
        digest = approval_digest(args.set_name, suffix, args.gap_ms, args.observe_ms,
                                 candidates, mode=UNATTENDED, delay_s=args.delay_s,
                                 observe_delay=args.observe_delay)
    else:
        digest = approval_digest(args.set_name, suffix, args.gap_ms, args.observe_ms, candidates)
    try:
        done = completed_bytes(args.log, args.dry_run, mode) if args.log else set()
    except (OSError, ValueError) as error:
        parser.error(f"log: {error}")
    plan = [c for c in candidates if c["value"] not in done]
    if args.max_trials:
        plan = plan[:args.max_trials]

    print(f"Set {args.set_name}: {len(candidates)} candidate(s), suffix 0x{suffix:02X}, "
          f"gap {args.gap_ms} ms, window {args.observe_ms} ms, mode {mode}"
          + (f", delay {args.delay_s:g} s" if args.unattended_rx_only or args.grouped else "")
          + (f", groups of {args.grouped}" if args.grouped else "")
          + (", passive observation through delays" if args.observe_delay else "")
          + f", digest {digest}")
    for c in skipped:
        print(f"  skip 0x{c['value']:02X} (rank {c['rank']}): {c['skip_reason']}")
    for c in candidates:
        if c["value"] in done:
            print(f"  skip 0x{c['value']:02X} (rank {c['rank']}): sent in {args.log}")
    print("  plan: " + " ".join(f"{c['rank']}:0x{c['value']:02X}" for c in plan))
    groups = None
    if args.grouped is not None:
        groups = make_groups(candidates, args.grouped)
        for group in groups:
            group["plan"] = [c for c in group["members"] if c["value"] not in done]
            print(f"  group {group['number']}: ranks {group['first_rank']}-{group['last_rank']}, "
                  f"{len(group['members'])} prefix(es)"
                  + (f", {len(group['plan'])} to send" if done else "") + ": "
                  + " ".join(f"0x{c['value']:02X}" for c in group["members"]))
    if args.list:
        return 0
    if not plan:
        print("Nothing to send.")
        return 0
    ask = input
    if args.auto_observe and not args.dry_run:
        parser.error("--auto-observe is only for --dry-run")
    if args.auto_observe and args.unattended_rx_only and args.grouped is None:
        parser.error("--auto-observe does not apply to --unattended-rx-only")
    if args.dry_run:
        service = MockService()
        if args.auto_observe:
            def ask(prompt):
                if "digit/track selection" in prompt:
                    answer = "ok dry-run"
                elif "start-group" in prompt:
                    answer = "start-group " + prompt.split("'start-group ", 1)[1].split("'")[0]
                elif "What did the changer do" in prompt:
                    answer = "t4"
                elif "front-panel check" in prompt:
                    answer = "panel-ok dry-run"
                else:
                    answer = "ready dry-run"
                print(prompt + answer)
                return answer
    else:
        if args.approve != digest:
            print(f"Refused: real runs need --approve {digest} for exactly this plan.")
            return 2
        if not args.log:
            print("Refused: real runs need --log.")
            return 2
        if input(f"Type {START_PHRASE} to transmit: ").strip() != START_PHRASE:
            print("Not confirmed; nothing sent.")
            return 2
        service = SocketService(args.socket)

    log = JsonLog(args.log if args.log else "/dev/null")
    sweep = Sweep(service, suffix, plan, gap_ms=args.gap_ms, window_ms=args.observe_ms,
                  delay_s=args.delay_s, log=log, ask=ask,
                  attended=not args.unattended_rx_only, observe_delay=args.observe_delay,
                  groups=groups)

    def interrupt(signum, frame):
        sweep.interrupted = True
        print("\nInterrupt: stopping after the current step; nothing is cancelled or resent.")

    previous = signal.signal(signal.SIGINT, interrupt)
    previous_term = signal.signal(signal.SIGTERM, interrupt)
    log.write({"event": "run", "set": args.set_name, "digest": digest,
               "suffix": f"0x{suffix:02X}", "gap_ms": args.gap_ms,
               "window_ms": args.observe_ms, "delay_s": args.delay_s,
               "dry_run": args.dry_run, "mode": mode,
               "observe_delay": args.observe_delay, "group_size": args.grouped,
               "plan": [f"0x{c['value']:02X}" for c in plan],
               "started": now()})
    try:
        verdict = sweep.run()
    finally:
        signal.signal(signal.SIGINT, previous)
        signal.signal(signal.SIGTERM, previous_term)
        log.close()
    return 0 if verdict in ("SET_COMPLETE", "SET_COMPLETE_RX_SILENT",
                            "SET_COMPLETE_GROUPED") else 1


if __name__ == "__main__":
    sys.exit(main())
