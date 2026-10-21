# H11 Rev.C — one attended Volume UP validation

> Historical review/procedure retained from the October 19 investigation.
> Reconciliation recorded 2026-10-21: see the [later operator reports](xs8-h11-warn-rc.md#later-operator-reports--reconciliation-recorded-2026-10-21),
> including successful attended trials and the subsequent uncertain STOP/fault.
> An earlier conditional PASS below is not renewed authorization after that fault.

Reviewed 2026-10-19 for candidate `3a8ffecd`. **PASS for one attended
`VOLUME_STEP UP` only after every prerequisite below is checked; otherwise
BLOCKED.** This is an operator-run proposal, not an executed test or approval for
held controls, CEC/Web restoration, MUTE or unattended use. No runtime code,
configuration, candidate binary, frame timing or retry policy is changed.

## Evidence and exact sequence

The operator reports the [DOWN trial physically passed](xs8-h11-warn-rc.md#receiver-connected-down-step--operator-report):
DOWN motion and independent cessation, four frames, no STOP/ROOT retry, five
global-only warnings, no fatal monitor fault, integrity still UNCERTAIN. This
supports the shared cleanup path but is not physical validation of UP on Rev.C.

| Order | Logical TX | Legacy TX | Purpose |
|---|---|---|---|
| 1 | `0x17` | `0x17` | Enter tested receiver context. |
| 2 | `0xF9` | `0x60` | Continuous volume UP. |
| 3 | `0x09` | `0x6F` | STOP, before context exit. |
| 4 | `0x07` | `0x1F` | Return to ROOT; not a substitute for STOP. |

The [socket command mapping](../xs8_service_core.cpp#L680-L683) selects UP.
[START](../volume_control.cpp#L163-L198) selects F9 for UP and E9 for DOWN;
[step](../volume_control.cpp#L276-L286) immediately requests cleanup after START,
with no added hold interval. Historical [physical observations](../research/notes/kenwood-xs-reverse-engineering-v18.md#L553-L562)
report continuous F9 UP and 09 stopping volume, followed by 07 returning ROOT.
These are historical operator observations, not newly replayed electrical logs.
[Protocol sequences](xs8-protocol.md#working-control-sequences) and
[directional step tests](../tests/volume_control_test.cpp#L291-L322) agree.
Legacy TX conversion is `logical = reverse8(~legacy)`; never apply it to RX.

## Obligations and residual risk

STOP is owed **before** START may reach the bus, even when START fails or its
waveform is uncertain. [Cleanup](../volume_control.cpp#L118-L160) attempts STOP
before ROOT. There are at most three attempts per cleanup invocation, not per
process lifetime. Any failed attempt latches a fault even after later success;
all failed STOP attempts preserve STOP/context debt and prevent ROOT. Fatal
capture failure prevents new commands but retains cleanup attempts; monitor-fault
handling and orderly shutdown can each attempt bounded cleanup again. No operator
retry or improvised recovery is permitted by this procedure.

New real-monitor service-path tests cover **both directions**, clean control,
global-only warnings in START, STOP and both, and a fatal per-line fault in STOP
after verified START. They assert exact byte order, warning confidence, retained
STOP debt, fault latching and rejection of subsequent commands. These are
synthetic GPIO tests, not motor feedback. Existing unit tests cover uncertain
entry/START/STOP/ROOT and cancellation separately.

Frame duration depends on the encoded byte; the DOWN physical duration must
not be assumed to apply exactly to UP. Neither nominal timing nor the previous
trial establishes a hard motor runtime bound. `--max-hold-ms 500` and the
inactivity watchdog run on the same owner
thread and cannot preempt blocked TX/verification. A successful GPIO send or
waveform MATCH never proves receiver execution. Power loss, process death,
electrical faults or an ignored STOP can leave motion unresolved; independent
physical power cutoff is required. See the [full safety review](xs8-h11-volume-safety-review.md).

## Mandatory preflight

- Use a **fresh** foreground service and new private socket, not the DOWN process
  carrying five warnings. Do not clear warning counters or receiver uncertainty
  to manufacture a baseline. Restart is not physical receiver recovery.
- Independently establish known normal receiver/ROOT and front-panel operation,
  stationary motor and no outstanding control uncertainty. If that cannot be
  established without speculative commands, the trial is BLOCKED.
- Speakers disconnected, silent input, volume manually set low but away from
  both end stops, especially the upper limit. Absolute volume is not known to
  software. Operator must continuously watch physical movement.
- Receiver power cutoff immediately reachable and able to remove receiver
  power. Agree on a conservative acceptable motion interval before arming; if a
  guaranteed physical runtime cap is required, use independent cutoff hardware.
- `kenwood-xs8.service` and all other GPIO owners must already be inactive.
  No Gateway, CEC, research, CD, sniffer or other command source may run against
  this test. Disconnect other XS8 peripherals with equipment powered off.
- Verify candidate checksum, existing FIFO/5 privilege and all baseline fields
  below. Unknown ownership, RT fallback, startup error, previous TX, warning or
  fault blocks the test. No production restart/configuration change is included.

### Exact launch and one-request client

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
XS8_VOLUME_TEST_DIR=$(mktemp -d /tmp/xs8-volume-up.XXXXXX)
chmod 700 "$XS8_VOLUME_TEST_DIR"
printf 'Test directory: %s\n' "$XS8_VOLUME_TEST_DIR"
XS8_GLOBAL_GAP_POLICY=warn XS8_RT_PRIORITY=5 ./xs8-service-h11-warn \
  --socket "$XS8_VOLUME_TEST_DIR/control.sock" \
  --allow-unrecovered-start --max-hold-ms 500 \
  > "$XS8_VOLUME_TEST_DIR/service.log" 2>&1
```

The process stays foreground; no research, chain, context-research or CD probe
is enabled. No new privilege, capability, scheduling policy wrapper or persistent
setting is introduced. Check its startup log: IDLE by operator authorization,
all four feature flags DISABLED, policy warn, no TX yet. Check private directory
and socket ownership/permissions. Do not start `xs8_sniff` alongside this owner.

Terminal B: set `XS8_VOLUME_TEST_DIR` to the **exact directory printed in A**.
Remain positioned to observe the receiver and remove power. Do not wait for the
client's three-second socket timeout if motion is unsafe. The script reads its
confirmation from `/dev/tty` because its source is supplied through a heredoc.
It sends no request at all if there is no attended terminal/confirmation.

```sh
umask 077
python3 - "$XS8_VOLUME_TEST_DIR/control.sock" <<'PY' 2>&1 | tee "$XS8_VOLUME_TEST_DIR/client.log"
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

if input("Speakers disconnected, cutoff ready, operator watching: type ONE_UP: ") != "ONE_UP":
    raise SystemExit("Cancelled; no request sent")
before = status()
required = {"state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
            "context_may_be_open": "0", "cd_state": "IDLE",
            "cd_release_owed": "0", "cd_lease_expired": "0",
            "last_byte": "NONE", "last_gpio_tx": "NONE",
            "receiver_integrity": "HEALTHY", "receiver_execution": "UNVERIFIED",
            "global_gap_policy": "warn", "global_gap_warnings": "0",
            "capture_integrity": "NO_KNOWN_GAPS"}
if any(before.get(key) != value for key, value in required.items()):
    raise SystemExit("Baseline not clean; no step sent; review without retry")
reply = request(b"VOLUME_STEP UP\n")
expected = ("OK VOLUME_STEP_TX direction=UP state=IDLE stop_owed=0 "
            "context_may_be_open=0 receiver_execution=UNVERIFIED")
if reply != expected:
    raise SystemExit("Step rejected/uncertain; no retry; ensure physical safety")
after = status()
final = {"state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
         "context_may_be_open": "0", "cd_state": "IDLE", "cd_release_owed": "0",
         "last_byte": "0x07", "last_gpio_tx": "TX_OK", "last_waveform": "MATCH",
         "last_stop_waveform": "MATCH", "receiver_execution": "UNVERIFIED",
         "global_gap_policy": "warn"}
if any(after.get(key) != value for key, value in final.items()):
    raise SystemExit("Final state uncertain; no retry; ensure physical safety")
warnings = int(after["global_gap_warnings"])
if (after.get("capture_integrity") != ("WARN_GLOBAL_GAP" if warnings else "NO_KNOWN_GAPS") or
        after.get("receiver_integrity") != ("UNCERTAIN" if warnings else "HEALTHY")):
    raise SystemExit("Confidence evidence inconsistent; no retry")
print("Software evidence recorded; physical movement and stop remain operator-observed.", flush=True)
input("Record physical outcome; ensure motor stopped before Enter for final STATUS: ")
status()
PY
```

There is exactly one state-changing request and no client retry, keepalive,
release or recovery command. Closing the client does not cancel a synchronous
step; the service must continue its required STOP cleanup. Review the final
STATUS and log for later faults rather than treating the script's exit as a
physical safety signal. Do not run this client twice to obtain a gap or a larger
movement. A trial with no gap is still an attended step trial, not new proof of
the warning path under hardware conditions.

## Evidence, abort conditions and shutdown

Normal trial evidence is exactly `17 F9 09 07`, MATCH for each local frame,
STOP evidence MATCH with zero retries, ROOT MATCH with zero retries, final IDLE,
monitor HEALTHY, STOP/context/CD debts zero. Retain full initial/final STATUS,
service/client logs, UTC trial time, checksum/manifest, monotonic frame times,
RT result/priority, lateness/clock diagnostics, individual warnings and cumulative
and per-TX warning counts. No fixed warning count is expected. A zero last-TX
warning count must not erase earlier warnings. If any warning occurs, require
`capture_integrity=WARN_GLOBAL_GAP` and `receiver_integrity=UNCERTAIN`; do not
interpret either HEALTHY monitor or MATCH as full capture certainty.

Record physical **UP movement and independent cessation** separately, including
approximate duration and normal front-panel operation. No extra XS8 command is
needed or permitted to prove recovery. No-gap success is still one physical
trial, not new evidence about the hardware warning path.

Abort on wrong direction, excessive/prolonged upward movement, approach to the
upper limit, unexpected TX, non-MATCH, STOP/ROOT failure/retry, fatal capture fault,
panel lock, RT/clock failure, missing reply or lost physical observation. A global-only
warning by itself is the experimental policy condition: preserve it and assess
all other evidence, never hide it or retry to obtain a clean run. If motion is
unsafe, **cut receiver power immediately**; do not wait for the three-second
socket timeout, watchdog, logs or software retries. GPIO LOW, Ctrl+C and process
exit do not themselves guarantee receiver STOP.

After safety is established, stop only the foreground candidate with Ctrl+C and
retain logs. Required orderly cleanup may send additional STOP/ROOT if debt
remains; do not suppress it. Never inject 07, 09, 15 or another motor/context
command as speculative recovery. Keep receiver power off if motor/context/front
panel state is uncertain. No production service restart, binary replacement or
persistent setting change is part of this trial. Do not run the client twice.
