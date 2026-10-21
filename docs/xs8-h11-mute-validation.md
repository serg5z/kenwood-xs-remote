# H11 Rev.C — attended MUTE ON/OFF validation

> Historical review/procedure retained from the October 19 investigation.
> Reconciliation recorded 2026-10-21: see the [later operator reports](xs8-h11-warn-rc.md#later-operator-reports--reconciliation-recorded-2026-10-21),
> including successful attended trials and the subsequent uncertain STOP/fault.
> An earlier conditional PASS below is not renewed authorization after that fault.

Review date: 2026-10-19. Candidate `3a8ffecd`.

## Verdict: PASS for two separately attended stages after preflight

The operator confirms the **red LED on the volume knob flashes when MUTE is ON
and does not flash when OFF**. This supplies the independent physical indication
missing from the previous review; it is operator evidence, not software readback.
Speakers remain disconnected. No H11 MUTE trial has been executed by this review.

This procedure permits exactly one ON-stage toggle, then an optional separately
confirmed OFF-stage toggle through the same foreground service. Stage 2 is never
automatic and is blocked by any uncertainty or fault in Stage 1. Any unmet
preflight condition makes the trial BLOCKED. No other restoration command is
included.

The operator reports TUNER, DOWN and UP trials physically passed. UP is recorded
here only at that supplied level: direction and cessation passed physical
observation; no new UP warning count, log path or timing is inferred. Those
results support proceeding to this separate review, not assuming MUTE works on
H11 or restoring CEC/Web access.

## Protocol, evidence and service path

| Order | Logical TX | Legacy TX | Interpretation |
|---|---|---|---|
| 1 | `0x17` | `0x17` | Enter tested receiver context. |
| 2 | `0xD8` | `0xE4` | MUTE toggle in that context. |
| 3 | `0x07` | `0x1F` | Context exit, not proof of front-panel recovery. |

Legacy TX converts with `logical = reverse8(~legacy)`; RX is not converted
using this formula. [Working sequences](xs8-protocol.md#working-control-sequences)
and the [October 12 physical report](../research/notes/xs8-context-video-mute-2026-10-12.md)
establish toggle semantics: an earlier production transaction muted; a later
attended context transaction restored audio while muted. The latter had
human-paced gaps and no archived raw transcript. The report also records ROOT,
front-panel and normal receiver-control checks after that experiment. It does
not establish universal recovery or an acknowledgment.

[MUTE dispatch](../xs8_service_core.cpp#L665-L679) uses
[VolumeControl::mute/context_key](../volume_control.cpp#L242-L274). BOOT_INHIBIT,
fault, cancellation, CD/research conflicts and unhealthy monitor remain guarded.
A caller's own volume hold can be cleaned up first, so this procedure demands
fresh IDLE and zero STOP/context/CD debt to avoid any volume cleanup frames.

The sole owner thread checks BUSY and transmits each frame, then performs
[local waveform verification](../xs8_service.cpp#L90-L137). Successful transaction
requires observed matching byte/tail evidence, not merely completed GPIO TX.
The context flag is set before entry; uncertain entry or D8 triggers exit
cleanup and fault latching. D8 is **not retried**. ROOT cleanup has at most three
attempts per invocation, with any failure latching a fault; failed exit retains
context debt. Shutdown can retry owed cleanup. A fatal monitor fault prevents
new commands and matching evidence but cannot guarantee physical receiver state.

From a clean baseline MUTE sets no motor STOP obligation and sends neither F9
nor E9 nor 09. A context flag being cleared means verified exit transmission,
not confirmed ROOT interpretation or cleared front-panel lock. `OK MUTE_TX`
is transaction evidence, not a physical muted-state result; logs must show all
three frames. Cancellation after entry can cleanly exit before D8, so the short
reply alone is insufficient evidence that the toggle was sent.

The receiver-state cache describes source and TAPE2 MONITOR, **not MUTE**.
Neither cached source, QUERY_STATUS nor last-byte 07 establishes mute state.
Global-only warnings may coexist with MATCH and HEALTHY monitor, but must remain
`WARN_GLOBAL_GAP` with receiver integrity UNCERTAIN. No software reply overrides
that reduced confidence or reports receiver execution as verified.

## Preflight and independent observation

1. Verify the exact checksum below and use the same approved identity and
   FIFO/5 privileges as the passed trials. No new privilege or persistent change.
2. Existing production service and every other GPIO owner must already be
   inactive; no CEC, Gateway, bridge, sniffer or competing socket client. Use a
   fresh foreground service/private socket with no prior TX or warnings, not a
   reused DOWN/UP process. Restart does not recover an uncertain receiver.
3. Speakers remain disconnected. Motor must be stationary, volume manually low,
   known normal receiver/front-panel operation and ROOT baseline established
   independently. Receiver power cutoff immediately accessible. Disconnect
   competing XS8 peripherals with equipment powered off.
4. Independently confirm the knob LED is **not flashing** initially; record
   photo/video where practical. Do not infer OFF from silence or software.
   If already flashing, do not toggle to manufacture a baseline: stop and obtain
   a separate plan. Keep motor stationary and power cutoff accessible throughout.
5. Confirm no residual STOP/context/fault uncertainty. Any unknown physical
   baseline, unavailable observation, owner conflict or failed checksum blocks
   transmission. The service authorization below asserts this external baseline;
   it does not discover it or perform startup recovery.

### Exact launch and two-stage fixed-request client

These are **operator-run instructions, not commands executed during development**.
First verify the already-staged candidate and its manifest against commit
`3a8ffecd985215a8a748d2bb788847756337e6f5` and this SHA-256:

```text
fc40aa188d65fe995788c59b4dc102d998dd3733de262fe2249f3e1fa403aaf2
```

Run `sha256sum -c SHA256SUMS-h11-warn` in the staging directory and compare the
manifest/checksum with this value. Confirm `kenwood-xs8.service` is already
inactive, and no fallback bridge, Gateway, CEC client or diagnostic can issue
commands or own GPIO. An active/indeterminate owner or missing physical setup
blocks launch; this procedure contains no production-service stop/restart step.

Terminal A, from that directory, using the same identity and approved RT
privileges as the successful FIFO/5 trial:

```sh
umask 077
XS8_MUTE_TEST_DIR=$(mktemp -d /tmp/xs8-mute.XXXXXX)
chmod 700 "$XS8_MUTE_TEST_DIR"
printf 'Test directory: %s\n' "$XS8_MUTE_TEST_DIR"
XS8_GLOBAL_GAP_POLICY=warn XS8_RT_PRIORITY=5 ./xs8-service-h11-warn \
  --socket "$XS8_MUTE_TEST_DIR/control.sock" \
  --allow-unrecovered-start --max-hold-ms 500 \
  > "$XS8_MUTE_TEST_DIR/service.log" 2>&1
```

The process stays foreground; no research, chain, context-research or CD probe
is enabled. No new privilege, capability, scheduling policy wrapper or persistent
setting is introduced. Check its startup log: IDLE by operator authorization,
all four feature flags DISABLED, policy warn, no TX yet. Check private directory
and socket ownership/permissions. Do not start `xs8_sniff` alongside this owner.

Terminal B: set `XS8_MUTE_TEST_DIR` to the **exact directory printed in A**.
Remain positioned to observe the receiver and remove power. Do not wait for the
client's three-second socket timeout if motion is unsafe. The script reads its
confirmations from `/dev/tty` because its source is supplied through a heredoc.
It sends no request at all if there is no attended terminal/confirmation.

```sh
umask 077
python3 - "$XS8_MUTE_TEST_DIR/control.sock" <<'PY' 2>&1 | tee "$XS8_MUTE_TEST_DIR/client.log"
import os
import socket
import stat
import sys
import time

path = sys.argv[1]
sys.stdin = open("/dev/tty", "r")
directory = os.stat(os.path.dirname(path))
endpoint = os.stat(path)
if (not stat.S_ISDIR(directory.st_mode) or
        stat.S_IMODE(directory.st_mode) != 0o700 or
        directory.st_uid != os.geteuid() or
        not stat.S_ISSOCK(endpoint.st_mode) or
        endpoint.st_uid != os.geteuid() or endpoint.st_mode & 0o077):
    raise SystemExit("Private socket ownership/permissions invalid; no request sent")

def request(command):
    print(f"client_mono_ns={time.monotonic_ns()} request={command.decode().strip()}", flush=True)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(3)
        client.connect(path)
        client.sendall(command)
        response = bytearray()
        while b"\n" not in response:
            chunk = client.recv(1024)
            if not chunk:
                raise RuntimeError("Disconnected; outcome uncertain; no retry")
            response.extend(chunk)
            if len(response) > 4096:
                raise RuntimeError("Oversized reply; no retry")
        line, remainder = bytes(response).split(b"\n", 1)
        if remainder:
            raise RuntimeError("Unexpected extra reply; no retry")
        text = line.decode("ascii")
        print(f"client_mono_ns={time.monotonic_ns()} reply={text}", flush=True)
        return text

def status():
    parts = request(b"STATUS\n").split()
    if not parts or parts[0] != "OK":
        raise RuntimeError("STATUS rejected; no retry")
    fields = {}
    for item in parts[1:]:
        key, value = item.split("=", 1)
        if key in fields:
            raise RuntimeError("Duplicate STATUS field; no retry")
        fields[key] = value
    return fields

def confirm(prompt, expected):
    if input(prompt) != expected:
        raise SystemExit("Cancelled; no further command; do not retry")

def check_final(fields, minimum_warnings=0):
    final = {"state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
             "context_may_be_open": "0", "cd_state": "IDLE",
             "cd_release_owed": "0", "cd_lease_expired": "0",
             "last_byte": "0x07", "last_gpio_tx": "TX_OK",
             "last_waveform": "MATCH", "receiver_execution": "UNVERIFIED",
             "global_gap_policy": "warn"}
    if any(fields.get(key) != value for key, value in final.items()):
        raise SystemExit("Unsafe/uncertain state; no further MUTE; ensure physical safety")
    warnings = int(fields["global_gap_warnings"])
    if warnings < minimum_warnings:
        raise SystemExit("Warning count regressed/service changed; no further MUTE")
    if (fields.get("capture_integrity") != ("WARN_GLOBAL_GAP" if warnings else "NO_KNOWN_GAPS") or
            fields.get("receiver_integrity") != ("UNCERTAIN" if warnings else "HEALTHY")):
        raise SystemExit("Confidence evidence inconsistent; no further MUTE")
    return warnings

confirm("LED NOT flashing, motor stationary, speakers disconnected, cutoff ready: type ONE_MUTE_ON: ",
        "ONE_MUTE_ON")
before = status()
required = {"state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
            "context_may_be_open": "0", "cd_state": "IDLE",
            "cd_release_owed": "0", "cd_lease_expired": "0",
            "last_byte": "NONE", "last_gpio_tx": "NONE",
            "receiver_integrity": "HEALTHY", "receiver_execution": "UNVERIFIED",
            "global_gap_policy": "warn", "global_gap_warnings": "0",
            "capture_integrity": "NO_KNOWN_GAPS"}
if any(before.get(key) != value for key, value in required.items()):
    raise SystemExit("Baseline not clean; no MUTE sent")
if request(b"MUTE\n") != "OK MUTE_TX":
    raise SystemExit("Stage 1 uncertain/rejected; Stage 2 blocked; no retry")
stage1_end = time.monotonic()
first_warnings = check_final(status())
confirm("Confirm LED NOW FLASHING, motor stationary; service log shows exactly 17 D8 07, "
        "each TX_OK/MATCH, ROOT retries=0, no faults/RT errors: type STAGE1_PASS: ",
        "STAGE1_PASS")
# The OFF stage is separated by at least three seconds after the ON reply.
remaining = stage1_end + 3.0 - time.monotonic()
if remaining > 0:
    time.sleep(remaining)
print("Stage 2 is optional. Cancellation leaves current MUTE state unchanged; no automatic restoration.", flush=True)
pre_confirm_warnings = check_final(status(), first_warnings)
confirm("LED STILL FLASHING, motor stationary, cutoff ready; separately authorize OFF: type ONE_MUTE_OFF: ",
        "ONE_MUTE_OFF")
# Recheck after the human pause; no stale readiness from the first stage.
pre_off_warnings = check_final(status(), pre_confirm_warnings)
if request(b"MUTE\n") != "OK MUTE_TX":
    raise SystemExit("Stage 2 uncertain/rejected; no retry or recovery toggle")
final_warnings = check_final(status(), pre_off_warnings)
confirm("Confirm LED STOPPED flashing, motor stationary; second 17 D8 07 all TX_OK/MATCH, "
        "ROOT retries=0 and no faults/RT errors: type STAGE2_PASS: ", "STAGE2_PASS")
check_final(status(), final_warnings)
print("Two stages recorded; receiver execution remains independently operator-observed, not acknowledged.", flush=True)
PY
```

## Expected evidence, aborts and restoration

Each separately confirmed stage sends exactly one `MUTE\n`; at most two in
total. STATUS reads send no XS8 frames. There is no automatic toggle-back, retry,
recovery, query, source selection or motor request. No command is sent on startup.
The three-second separation starts after the Stage 1 reply, later than its TX.
Stage 2 requires successful Stage 1 software checks, explicit physical/log review,
the wait, another health check, `ONE_MUTE_OFF`, and a fresh post-confirmation check.
Inspect service.log from a separate terminal without sending commands. If the
required per-frame/RT evidence cannot be established, do not type STAGE1_PASS.
A timeout/disconnect is an uncertain outcome: do not resend the toggle.

Retain checksum/manifest, UTC trial time, initial/final STATUS and complete
service/client logs. Require normal `17 D8 07`, TX_OK and MATCH for each frame,
ROOT retry count zero, final IDLE/monitor HEALTHY, no STOP/context/CD debt.
Last-byte 07/MATCH covers only ROOT; retain the separate D8 observation. A retry,
missing D8 or uncertain waveform is not a normal successful trial. Record all
sequence/warning counters, per-TX/cumulative integrity, RT/lateness/clock results.
No fixed warning count is expected. Cumulative warnings remain visible even if
ROOT's per-TX warning count is zero. With any global warning require
`capture_integrity=WARN_GLOBAL_GAP`, `receiver_integrity=UNCERTAIN` and
`receiver_execution=UNVERIFIED`.

Record **Stage 1: LED begins flashing**, **Stage 2: LED stops flashing**, and
stationary motor separately, with stage times and normal front-panel responsiveness. A
safe front-panel observation must not dispatch another XS8 control or become an
unapproved audio-restoration step. If panel recovery cannot be independently
checked within the safe setup, record it as unverified; 07/MATCH cannot replace
that check. Electrical frame origin/execution remain unacknowledged.

Abort on motor motion, unexpected command/extra toggle, missing or non-MATCH
frame, ROOT failure/retry, fatal capture fault, panel lock, RT/clock failure,
lost observation or missing reply. A global-only warning alone is not an abort
if every other condition holds: preserve it and the UNCERTAIN confidence. For
unsafe physical behavior use the independent receiver power cutoff immediately;
do not wait for the client timeout or software cleanup. No speculative 07, 09,
15, repeated MUTE or other recovery sequence is authorized.

After physical safety is established, stop the foreground candidate with Ctrl+C
and retain logs. Orderly shutdown may attempt owed context cleanup; it must not
repeat D8. Do not restart production, overwrite binaries or alter configuration.
If context/panel/motor state is uncertain, leave receiver power off and investigate.
Software cannot guarantee recovery after process death, power loss or electrical
failure.

The separately confirmed Stage 2 is the only authorized second MUTE. It is not
a recovery action: any Stage 1 uncertainty blocks it. Cancellation, EOF, Ctrl+C,
timeout or failed checks terminate the client without a compensating toggle.
If it exits after Stage 1, MUTE may remain ON; do not rerun this client or send
another command. Any subsequent restoration requires a separate review and
explicit approval based on the independently observed state.

## Offline checks

`python3 -m unittest tests/test_xs8_mute_validation_client.py` runs the documented
workflow with fake requests, STATUS, clock and confirmations; it opens no socket
and never reads the operator terminal. It tests the three-second wait, explicit
stage confirmations, warning preservation, fault rechecks and no retry after
uncertain results. The actual service/monitor MUTE path is separately covered by
`tests/test_xs8_service_monitor.py`. Client shell/Python blocks are syntax-checked
without executing the operator commands.
