# H11 Rev.C warn candidate: volume START/STOP safety review

> Historical review/procedure retained from the October 19 investigation.
> Reconciliation recorded 2026-10-21: see the [later operator reports](xs8-h11-warn-rc.md#later-operator-reports--reconciliation-recorded-2026-10-21),
> including successful attended trials and the subsequent uncertain STOP/fault.
> An earlier conditional PASS below is not renewed authorization after that fault.

Reviewed and followed up 2026-10-19, candidate
`3a8ffecd985215a8a748d2bb788847756337e6f5`. Follow-up changes are offline tests
and documentation only; no production code, staged candidate binary, timing,
retry policy or configuration changed. No hardware test was executed here.

## Verdict: PASS for one attended DOWN step

The initial review was BLOCKED because the real-monitor harness covered only
source selection and the proposed physical cutoff setup was unspecified. The
follow-up now exercises START/STOP warnings and a fatal capture fault after a
verified START through the real service/monitor/decoder/verifier. The operator
has specified speakers disconnected, volume slightly above minimum, immediately
accessible receiver power cutoff, exclusive GPIO ownership, private socket and
no Gateway/CEC/other command sources.

**PASS applies only to one `VOLUME_STEP DOWN` using the procedure below, after
operator preflight.** Any unmet prerequisite makes execution BLOCKED. This is a
bounded command experiment with an independent attended power-cutoff fallback,
not a guaranteed physical runtime bound. It does not authorize held controls,
UP, MUTE testing, production restoration or unattended operation.

The [TUNER trial](xs8-h11-warn-rc.md#receiver-connected-tuner-result--operator-report)
physically validated an input change, not a motor stop. The new tests establish
software behavior under synthetic faults, not receiver execution. No software
timer, GPIO TX result or matching waveform guarantees motor shutdown.

The subsequently reported DOWN physical PASS and the separately reviewed
[one-step UP proposal](xs8-h11-volume-up-validation.md) are recorded in the
[RC results](xs8-h11-warn-rc.md). The DOWN instructions below remain scoped to DOWN.

## Physical protocol evidence

All command values in this review are **logical TX**, not RX labels.

| Purpose | Sequence / legacy TX | Evidence and limits |
|---|---|---|
| Enter the tested receiver context | `0x17` / legacy `17` | Observed context-dependent interpretation; no context-entry acknowledgment. |
| Continuous volume DOWN | `0xE9` / legacy `68`, after `0x17` | Reported physical continuous decrease. |
| Continuous volume UP | `0xF9` / legacy `60`, after `0x17` | Reported physical continuous increase; excluded from the proposed test. |
| Volume STOP | `0x09` / legacy `6F`, before exit | Reported to stop active volume adjustment; isolated no-RX behavior is not a safety proof. |
| Context exit | `0x07` / legacy `1F`, after STOP | Restores the tested ROOT interpretation; not a substitute for STOP or proof that front-panel locks are cleared. |

The [registry](../research/COMMAND_REGISTRY.md#documented-sequences) and
[protocol sequences](xs8-protocol.md#working-control-sequences) preserve the
Arduino-era hardware observations. The [coverage record](../research/XS8_COMMAND_COVERAGE.md)
also records a physical DOWN/STOP trial with no additional RX response frames;
its raw trace was not deposited. Thus there is no established STOP execution ACK.
No TX conversion formula is applied to received bytes here.

Earlier operator reports in the project conversation described continued motor
movement after STOP/ROOT logged TX_OK, and scheduler lateness associated with
malformed local frames. Those reports motivate this review; they are not newly
reproduced H11 Rev.C measurements or proof of a specific root cause. The current
TUNER result cannot resolve whether a correctly observed STOP is always executed.

## Confirmed implementation protections

| Mechanism | Actual behavior / source |
|---|---|
| Startup | Service defaults to BOOT_INHIBIT with no automatic recovery. Explicit authorization asserts an externally checked baseline; it does not inspect receiver context/motor state. [Construction](../xs8_service_core.cpp#L88-L103). |
| Entry and START | `context_may_be_open` is set before `0x17`. `stop_owed` is set **before** `0xE9/0xF9` may reach the bus, including failed/uncertain START. [State transitions](../volume_control.cpp#L163-L198). |
| Cleanup order | STOP `0x09` precedes ROOT `0x07`. No ROOT is attempted after all STOP attempts fail; STOP/context obligations remain. [Cleanup](../volume_control.cpp#L118-L160). |
| Retry scope | Up to **three attempts per cleanup invocation**, not three retries plus the first attempt. Any failed attempt causes FAULT_LATCHED even if a later attempt succeeds. Entry and START are not automatically resent. [Retry helper](../volume_control.cpp#L30-L41). |
| Failure state | Successful waveform-level STOP clears `stop_owed`; successful ROOT clears the context flag. ROOT failure can therefore leave STOP cleared and context open. These are software obligations, not physical-state measurements. |
| Subsequent cleanup | Fault latching prevents new starts. Monitor-fault handling, an owning client disconnect and final orderly shutdown can invoke cleanup again if obligations remain. Three attempts is **not** a lifetime/process-wide transmission cap. [Tick/shutdown](../xs8_service_core.cpp#L824-L909). |
| Signals | SIGINT/SIGTERM set a flag; normal execution performs cleanup. A frame sleep retries EINTR and is not immediately cancelled by the flag. SIGKILL/crash/power loss cannot guarantee cleanup. [Sleep](../xs8_gpio.c), [shutdown](../xs8_service.cpp#L1227-L1243). |

### Runtime and watchdogs: no hard physical motor bound

The service inactivity timeout is **1200 ms**. Its CLI default max hold is
**20000 ms**, configurable within **100–20000 ms**; the old direct GPIO bridge
defaults to **2000 ms**, a different executable. Current Pi process arguments
were not inspected and must not be inferred from those defaults.
[Service settings](../xs8_service.cpp#L275-L303),
[inactivity](../xs8_service_core.cpp#L88-L99),
[fallback defaults](../main.cpp#L20-L24).

Same-direction repeats refresh inactivity but do not reset the original hold
timestamp. Watchdog cleanup blocks rearming until the appropriate release;
stale events are rejected. The original timestamps supplied to `start()` are
retained across synchronous entry/START TX, not sampled anew at physical motion.
[Press/watchdog](../volume_control.cpp#L193-L219),
[watchdog](../volume_control.cpp#L325-L334).

The watchdog **initiates cleanup when the owner thread runs**. The same thread
performs TX, monitoring and socket work; it cannot service release/watchdog
checks while blocked in a send, verification or logging. Socket poll cadence is
nominally 20 ms, not a scheduling guarantee. A send can include a 100 ms BUSY-free
timeout, waveform transmission and the existing 300 ms observation timeout.
The RT frame limit of 250 ms is checked after wakeups; it cannot preempt a
descheduled thread or guarantee a physical deadline. Retries add further delay.
[GPIO timing](../xs8_gpio.c), [TX](../xs8_gpio.c),
[verification](../xs8_service.cpp#L90-L137), [poll](../xs8_service.cpp#L632-L640).

Even nominal values must not be presented as physical limits. From the current
constants, DOWN's frame is about 114.6 ms and STOP's about 99.9 ms. An atomic
DOWN step's four frames total about 429 ms, before BUSY guards, scheduling,
observation and logging overhead. These are **calculated nominal wire durations**,
not measured motor runtimes. The receiver can start/stop at unobserved points.

`VOLUME_STEP DOWN` adds no hold: it requests STOP as soon as START verification
returns. It does not wait for the max-hold timer, nor can that timer interrupt
the synchronous step. START observation delays still postpone STOP. A configured
500 ms limit is not a promise that motion ceases within 500 ms.
[Step](../volume_control.cpp#L276-L286).

## Gap/fault outcomes during motor activity

| Condition | Software outcome | Physical uncertainty |
|---|---|---|
| Global-only warning during entry or START, unique matching valid local frame | `send()` can succeed, START can continue, receiver confidence becomes UNCERTAIN. Warning alone does not latch a motor fault or force immediate STOP. | A valid delivered waveform does not establish electrical completeness or execution. |
| Global-only warning during STOP, otherwise unique MATCH | STOP can succeed and clear `stop_owed`; ROOT is then attempted. No retry is triggered solely by the warning. | This intentionally accepts reduced capture confidence. It does not measure whether the motor stopped. |
| Invalid/missing/mismatching/ambiguous START or STOP waveform | START uncertainty invokes STOP cleanup; STOP uncertainty retries and then faults. Strict final DATA-before-BUSY tail checks still apply in warn mode. | The receiver may already be moving even when START reports failure. |
| Fatal capture fault discovered while held | `tick()` invokes volume shutdown once for that monitor failure; new commands are blocked. GPIO STOP is still attempted through cleanup. | Failed manual monitor cannot resume verification: further local sends cannot get MATCH, even if their GPIO routine succeeds. |
| Fatal fault during a synchronous step | Failure return follows the existing START/STOP cleanup path; a later owner-loop tick may invoke monitor-fault cleanup again. | Detection/cleanup is delayed until the synchronous work returns. |
| All STOP attempts unverified | FAULT_LATCHED, STOP owed/context retained; no ROOT. Later disconnect/shutdown may attempt cleanup again, without clearing the monitor failure. | Actual STOP delivery/execution remains unknown; physical intervention is required. |
| RT restore/demotion unrecoverable | Driver releases electrical outputs LOW and exits the process. | Output release is not an XS8 STOP frame. Software cleanup may never run. [RT fatal path](../xs8_gpio.c). |

The key acceptance rule is `gpio_completed && waveform==MATCH`, not receiver
confidence. Warning propagation does not add a second motor authorization gate.
[Transmitter](../xs8_service.cpp#L90-L137),
[monitor-fault response](../xs8_service_core.cpp#L884-L892),
[strict tail check](../xs8_decode.c#L133-L141).

## Clients and serialization

- The service serializes frames and the atomic step on one owner thread. A step
  does not depend on a browser release, client heartbeat or keeping its client
  connected; STOP stays inside the same service operation.
- CEC client uses connection-owned PRESS/RELEASE, repeats, age checks, a bounded
  queue and separate inactivity/max-hold guards. Disconnect does not silently
  resume a hold. These protections do not interrupt an in-flight service TX.
  [CEC core](../cec_client_core.cpp#L71-L163).
- Gateway supports both steps and connection-owned Web holds; keepalives refresh
  inactivity. Client-side release/timeout is not a receiver ACK. Do not use a
  browser or CEC button for the first motor experiment.
  [Gateway step](../xs8_gateway.py#L585-L622),
  [volume ownership](../xs8_service_core.cpp#L450-L510).
- An unrelated client cannot take over/release another client's active hold.
  Source/CD/research operations are gated against conflicting work, but other
  production commands are still available on an authorized IDLE socket. The
  candidate is not a DOWN-only service; private-socket isolation is mandatory.
- Serialization is not an experiment lock: a queued CEC/Web command can run
  after the step completes and may start another hold. A direct PRESS carries
  no original CEC timestamp over the socket; client-side age checks are not an
  authorization barrier for an unrelated client. Keep the private socket isolated.
- The fallback `cec-xs-bridge` is a different GPIO owner: its transmitter reports
  raw GPIO completion rather than using the service's waveform verifier.
  Its optional monitor is not an equivalent safety path. It must be inactive.
  [Fallback transmitter](../main.cpp#L43-L51).

## Test evidence and remaining coverage limits

Existing state-machine coverage and the native suite were rerun in the follow-up:

- [Volume tests](../tests/volume_control_test.cpp#L72-L175): partial START,
  repeated STOP failure, exit failure, direction change, shutdown and watchdogs.
  [Step tests](../tests/volume_control_test.cpp#L291-L405) check frame ordering,
  pre-TX obligations, no added hold, cancellation and failure at each stage.
- [Service tests](../tests/xs8_service_core_test.cpp#L209-L254) exercise failed STOP
  verification, disconnect and a monitor failure during a hold; repeated ticks
  do not create an unbounded cleanup loop.
  [Atomic step tests](../tests/xs8_service_core_test.cpp#L1228-L1380) cover
  interlocks, uncertain STOP/ROOT and fault retention. They use FakeTx.
- [Socket tests](../tests/test_xs8_service_socket.py#L378-L467) check competing
  owners, lost connection and cleanup, but their monitor is a fake backend.
- [Real-monitor service tests](../tests/test_xs8_service_monitor.py) now link the
  actual service, monitor, decoder and verifier to synthetic GPIO events. The
  production GPIO driver is not linked. [Monitor regressions](../tests/xs8_monitor_test.c#L368-L745)
  still reject missing edges and incompatible local tails in strict/warn modes.

| New service-path scenario | Verified result |
|---|---|
| Clean atomic DOWN step | Exactly `17 E9 09 07`; STOP/ROOT MATCH; IDLE; obligations clear. |
| Global-only gap during START | Warning logged; START MATCH; required STOP/ROOT still complete; cumulative confidence WARN and receiver integrity UNCERTAIN. |
| Global-only gap during STOP | STOP MATCH without a warning-induced retry; ROOT follows; debt clears; warning evidence remains visible. |
| Warnings during both START and STOP | Two cumulative warnings; same normal order and cleanup. Final ROOT has zero per-TX warnings without erasing earlier warnings. |
| Fatal per-line fault during STOP after verified START | Monitor FAILED; GPIO sends can complete but STOP is NOT_OBSERVED. FAULT_LATCHED with STOP/context still owed; no ROOT or second START. Later volume, MUTE and source commands rejected. |

For the fatal scenario the test observes three STOP attempts in the step's
cleanup, three in the one-shot monitor-fault handler, and three at final orderly
shutdown: **nine**, not an unbounded loop or a process-wide limit of three.
The synthetic FIFO retains unread edges when the failed monitor stops consuming
while GPIO cleanup continues. No fake "recovered MATCH" is substituted.

Validation at the original DOWN review boundary: **9 real-monitor integration tests PASS**, including the original
source/attribution cases and both new test methods with their subcases. Native
`make test` PASS with one existing readline-related skip; the real-monitor test
executable builds with `-O2 -Wall -Wextra -Werror`. No hardware binary was executed.

These are not exhaustive physical/fault coverage. Entry/ROOT-specific warnings,
volume-phase read errors, cancellation and competing clients are not all crossed
with the real-monitor volume trace; existing unit/monitor/socket tests cover
those mechanisms separately. Broader combined coverage and held-client trials
remain future gates before production restoration, not claims of this one-step
PASS. Electrical loss before numbering, delayed events and receiver execution
remain outside what synthetic tests establish.

## Smallest proposed experiment — operator preflight required

One attended DOWN step only; no hardware action is performed by this document:

1. Reuse the exact checksummed candidate and previously validated FIFO/5
   privileges, without changing binaries, units, symlinks or persistent settings.
   Keep all research/chain/probe flags off. Establish known ROOT and normal
   front-panel operation independently; an old context/STOP uncertainty blocks
   the test. Service restart is not receiver recovery.
2. Power down before wiring; disconnect/power off CD and other XS8 peripherals.
   Isolate GPIO ownership and use a fresh owner-only socket as in the
   [TUNER procedure](xs8-h11-connected-readiness.md). No CEC/Web/other test client
   may target it. The operator must attend throughout.
3. Speakers must be disconnected. Use a silent input and a small, manually
   established low-volume margin above the lower end stop, so DOWN is observable
   without deliberately driving into the mechanical stop. Confirm motor stationary.
4. Agree on an acceptable physical motion interval and a genuinely independent,
   reachable receiver power-disconnection method before arming. If a strict
   physical runtime cap is required, a human reaction/daemon timer is insufficient;
   an independent cutoff must enforce it. Do not claim a 500 ms software limit
   supplies this protection. The operator has specified an immediately accessible
   power cutoff; verify accessibility and ability to remove receiver power before
   the trial. This review has not physically validated that arrangement.
5. Fresh service, private socket, `--max-hold-ms 500` as a backstop only. Require
   IDLE, healthy monitor, no STOP/context/CD obligations, no prior TX and no
   warning/fault at the baseline. No automatic recovery or query to establish it.
6. After separate operator authorization, send exactly **one** service request
   `VOLUME_STEP DOWN`. Expected normal frame sequence is precisely
   `0x17 → 0xE9 → 0x09 → 0x07`, no added hold. No raw research send, chain, UP,
   held PRESS, scan or second trial. Do not retry the request after timeout.
7. Independently observe a brief downward movement and physical stop. Capture
   per-frame TX/timing, STOP evidence, warning/fatal diagnostics, reply and final
   STATUS. Read-only STATUS is not proof of motion stopping. Do not send SYNC,
   source/MUTE/CD commands to test recovery during this experiment.

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
XS8_VOLUME_TEST_DIR=$(mktemp -d /tmp/xs8-volume-down.XXXXXX)
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

if input("Speakers disconnected, cutoff ready, operator watching: type ONE_DOWN: ") != "ONE_DOWN":
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
reply = request(b"VOLUME_STEP DOWN\n")
expected = ("OK VOLUME_STEP_TX direction=DOWN state=IDLE stop_owed=0 "
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

### Required evidence

Collect candidate checksum/commit, launch identity/options and UTC trial time;
`service.log`, `client.log`, initial/final STATUS; the four intended TX bytes,
decoded bus observations and monotonic timestamps; RT result and lateness/clock
statistics; STOP reason, evidence and retry count; ROOT outcome; raw warning
counters/sequence details and fatal diagnostics. Cumulative warnings may be
nonzero while the last-TX warning count is zero (the last TX is ROOT).
Record physical direction, cessation, approximate movement duration and final
front-panel operability separately, without sending another XS8 command.

Successful software evidence requires normal four-frame order, MATCH for each,
IDLE with no owed STOP/context, and monitor HEALTHY. Preserve `rt=FIFO`, the
requested priority 5, all timing/lateness statistics and zero clock errors; an
RT fallback/restore failure is not a successful FIFO/5 trial. If a warning occurs,
it must stay visible and receiver integrity must become UNCERTAIN. Physical motion and cessation must be
recorded separately. A final last-byte `0x07`/MATCH does not prove STOP: preserve
the separate STOP evidence and the operator's observation. Even a physical PASS
would apply only to that one trial, not held UP/DOWN or unattended operation.

### Stop and recovery conditions

Unexpected/increasing/prolonged motion, non-MATCH, failed STOP/ROOT, capture
failure, unexpected command, panel lock, missing response or lost observation
ends the trial. Do not wait for a socket timeout, log message or retry budget if
physical motion is unsafe. Use the pre-agreed independent receiver power cutoff;
SIGINT, GPIO LOW and process exit are not receiver STOP acknowledgments.

Do not bypass fail-closed policy or inject `0x07`, `0x09`, `0x15`, CD STOP or
further context commands to improvise recovery. After physical safety is
established, stop the foreground candidate and retain its log. Orderly cleanup
may make additional required STOP/ROOT attempts; do not suppress those paths.
No production service is restarted as part of this proposal.

Keep receiver power off if motor, front panel or context state is uncertain.
Inspect wiring and establish physical recovery separately before any restart or
reconnection; the software fault must not be cleared by an unreviewed restart.
No universal recovery sequence or guaranteed motor shutdown is established.

## Later validation record — 2026-10-21

The original nine-test count above is a historical review snapshot. The retained
suite now includes ten real-monitor service-path cases, including UP frame
ordering. Later operator reports independently confirmed attended DOWN and UP
motion and cessation, and MUTE ON/OFF by the flashing knob LED. Later CEC
integration nevertheless encountered an uncertain STOP and conservatively
latched a fault; see the [RC reconciliation](xs8-h11-warn-rc.md#later-operator-reports--reconciliation-recorded-2026-10-21).
These reports do not authorize unattended operation or resolve execution of
that particular STOP frame.
