# H11 Rev.C — CEC restoration readiness

> Historical review/procedure retained from the October 19 investigation.
> Reconciliation recorded 2026-10-21: see the [later operator reports](xs8-h11-warn-rc.md#later-operator-reports--reconciliation-recorded-2026-10-21),
> including successful attended trials and the subsequent uncertain STOP/fault.
> An earlier conditional PASS below is not renewed authorization after that fault.

Review: 2026-10-19, unchanged warn candidate `3a8ffecd`.

## Verdicts and limits

- **PASS for one bounded, attended foreground CEC integration session**, subject
  to the installed-state inspection, identity checks and physical preflight below.
  Missing or conflicting prerequisites make execution BLOCKED.
- **BLOCKED for unattended production restoration.** Held/repeated-key behavior
  on H11 has not been physically validated; installed binary/unit/settings have
  not been supplied for comparison; no unattended motor-control approval exists.

Operator-confirmed connected TUNER, DOWN, UP and MUTE ON/OFF trials all passed
physical observation and TX_OK/MATCH. Global warnings coexisted with HEALTHY
monitor and UNCERTAIN receiver integrity. The red knob LED independently showed
MUTE ON (flashing) and OFF (not flashing). These observations validate those
attended transactions, not held buttons, restart recovery or guaranteed STOP.
No hardware was accessed for this review; no configuration or runtime code changed.

## Audited execution path

| Component | Finding / source |
|---|---|
| CEC intake | [Callback/main loop](../cec_xs_client.cpp): volume up/down and mute only; duration 0 means press/repeat, positive means release. Local steady-clock timestamps, bounded 32-event queue, nominal 20 ms loop. Adapter opens once; no demonstrated automatic mid-run adapter reconnection. |
| Client translation | [Core](../cec_client_core.cpp): fresh press opens service socket; sends VOLUME_PRESS, repeated same-direction presses refresh it; release sends VOLUME_RELEASE then closes. MUTE latches until CEC release and sends once; uncertain MUTE is not automatically retried. |
| Socket transport | [Transport](../cec_service_socket.h): bounded replies, 500 ms connect wait and nominal three-second command deadline. Synchronous waits postpone client processing; these are not hard physical runtime bounds. No XS8 implementation or GPIO access in client. |
| Service admission | [Command/ownership handling](../xs8_service_core.cpp): multiple clients, one TX owner thread. Existing research/CD conflicts, unhealthy monitor, startup inhibition and fault rules remain. STATUS performs no requested TX, although tick can service pre-existing cleanup obligations. |
| Volume start | [VolumeControl](../volume_control.cpp): context debt before 17; STOP debt before F9 (UP) or E9 (DOWN), even if send subsequently fails. Repeats refresh inactivity without another START or resetting absolute hold start. Direction change cleans STOP/ROOT before new START. |
| Termination | Release, inactivity, absolute cap, owning connection EOF and orderly shutdown attempt 09 before 07. Three attempts per cleanup invocation; failed attempts latch a fault. Exhausted STOP retains debt and prevents ROOT. Further bounded fault/shutdown cleanup can occur. |
| Evidence | [Service transmitter](../xs8_service.cpp): checkpoint before send, strict byte/tail/window verification after send. GPIO completion, observed MATCH and receiver execution are distinct. Neither TX_OK nor MATCH acknowledges STOP execution. |
| Warn policy | Global-only forward gaps can warn without poisoning an otherwise valid frame. Per-line/global regressions, line gaps, timestamp/read faults and detected overflow remain fatal. [Policy](xs8-global-gap-policy.md). Cumulative warnings remain visible and receiver confidence becomes UNCERTAIN. |

Both client and service use 1200 ms inactivity. The service default max hold is
20000 ms, as is the repository client systemd template. The prior 500 ms setting
was a **test override**, not evidence of the currently installed service value.
A repeat extends inactivity, not absolute MAX_HOLD. On watchdog expiry the
service blocks rearming until release/disconnect; the client quarantines expired
or uncertain holds. It does not intentionally resume a hold after socket loss.
CEC timestamps are **not** forwarded over the socket: service command receipt
gets a new timestamp. Client age filtering is the first stale-event defense.

A missing release with no repeats should invoke inactivity; continuing repeats
without release reaches the absolute cap. Timers, socket EOF and cleanup share
the synchronous service thread: a blocked send/verification, scheduler stall or
logging delay postpones them. The 500 ms test cap is not a guaranteed 500 ms
physical stop. SIGINT/SIGTERM permit normal cleanup; SIGKILL, crash, power loss,
loss of ground or driver/wiring failure can defeat it.

### Warning and failure implications

A global warning during START does not remove STOP debt. A warning during STOP
can still yield MATCH and clear software debt; receiver integrity remains
UNCERTAIN. This is intentional experimental behavior, not a guarantee the motor
stopped. A receiver that ignores a correctly observed STOP can continue running
without a software fault. Warn mode also cannot detect every electrical edge
lost before sequence accounting. These are residual limits, not a reproduced
new CEC defect. A fatal monitor fault during activity invokes owed cleanup,
latches fault when STOP cannot verify, and rejects subsequent commands. Cleanup
may electrically transmit even when failed monitoring prevents verification.

The startup/restart limit is material: a repeat first delivered after a client
restart can look like a fresh press. Empty queues and no startup TX do not prove
physical release. Automatic systemd restarts and adapter recovery need separate
attended validation before unattended use. Do not deliberately restart during
motor movement for this first integration test.

## Installed-versus-candidate comparison: incomplete by design

Only the repository template and staged artifacts are available here. No live
`systemctl cat`, process command line, production checksum, effective RT limits
or drop-in configuration was supplied. Therefore exact installed differences
are **UNVERIFIED**, not assumed equal to the template.

Known candidate: ARMv6 hard-float, H11 pins 6/19 TX and 13/26 RX, default strict;
explicit process environment selects warn/FIFO priority 5. SHA-256:

```text
fc40aa188d65fe995788c59b4dc102d998dd3733de262fe2249f3e1fa403aaf2
```

Known repository client unit: user serg, default socket /tmp/kenwood-xs8.sock,
max hold 20000, Restart=on-failure, five-second delay. There is **no repository
kenwood-xs8.service template establishing the installed service ExecStart**.
Older advice in cec-client-systemd.md mentions a single-client socket; current
service is multi-client. Do not use that text as evidence of present behavior.
The old direct cec-xs-bridge owns GPIO itself: never run it beside xs8-service,
and do not assume its executable/settings match this H11 candidate.

### Operator inspection only — before launch

These commands have not been run here. Run on the Pi, save output privately:

```sh
systemctl cat kenwood-xs8.service
systemctl show kenwood-xs8.service -p FragmentPath -p DropInPaths -p ExecStart -p User -p Group -p MainPID -p ActiveState -p Restart -p LimitRTPRIO -p LimitRTTIME
systemctl cat kenwood-cec-xs-client.service
systemctl show kenwood-cec-xs-client.service -p ExecStart -p User -p MainPID -p ActiveState -p Restart
pgrep -af 'xs8-service|cec-xs|xs8_sniff|xs8_stimulus|xs8_send|xs8_gateway'
```

Inspect environment files/drop-ins locally for XS8_GLOBAL_GAP_POLICY,
XS8_RT_PRIORITY, socket, authorization/recovery flags and max hold. Do not share
credentials or unrelated environment secrets. Resolve the actual executable
paths from ExecStart and /proc/PID/exe, then run `sha256sum` on those exact files;
record executable and configuration hashes. Compare their source/build identity
with the candidate manifest. Inspect client libCEC/loader dependencies with
`file`, `readelf -h -A -l -d` and trusted-artifact dependency checks before use.
Missing units, ambiguous owners, unexpected recovery flags or unavailable
privileges block the test; no stop/restart action is hidden in these checks.

## Exact attended staging and launch

Use only the already approved staging directory containing the candidate,
`cec-xs-client`, manifest and checksums. No copy over production binaries,
symlink replacement, unit install, enable or restart is authorized here. If the
client artifact is not already staged and identity-verified, stop and arrange a
separate reviewed staging action; do not improvise a different CEC frontend.

Preflight: speakers disconnected, silent input, low volume away from end stops,
stationary motor, normal ROOT/front-panel baseline independently established,
receiver power cutoff within reach. Record knob LED initial state. Keep remote
buttons released before either process starts. Production service, all CEC
frontends, Gateway and GPIO diagnostics must already be inactive. If not, this
procedure is BLOCKED pending a separately approved maintenance action. No other
client may send controls to the private socket. Never launch a passive sniffer
beside the monitor owner.

Terminal A, same operator/approved GPIO and FIFO/5 permissions as passed trials:

```sh
sha256sum -c SHA256SUMS-h11-warn
sha256sum -c SHA256SUMS
cat manifest.json
umask 077
XS8_CEC_TEST_DIR=$(mktemp -d /tmp/xs8-cec-h11.XXXXXX)
chmod 700 "$XS8_CEC_TEST_DIR"
printf 'Test directory: %s\n' "$XS8_CEC_TEST_DIR"
XS8_GLOBAL_GAP_POLICY=warn XS8_RT_PRIORITY=5 ./xs8-service-h11-warn \
  --socket "$XS8_CEC_TEST_DIR/control.sock" \
  --allow-unrecovered-start --max-hold-ms 500 \
  > "$XS8_CEC_TEST_DIR/service.log" 2>&1
```

Compare candidate hash with the exact value above, not just a colocated checksum
file. This is operator authorization of a known baseline, not automatic startup
recovery. Research, chains, context research and CD probes stay disabled. Keep
this foreground process alive; do not reuse a previously faulted test process.

Terminal B: assign XS8_CEC_TEST_DIR to the exact printed directory. Before
starting CEC, perform this read-only STATUS check; repeat the same command after
each separately approved physical action (it sends no control request):

```sh
python3 - "$XS8_CEC_TEST_DIR/control.sock" <<'PY'
import socket, sys, time
with socket.socket(socket.AF_UNIX) as s:
    s.settimeout(3)
    s.connect(sys.argv[1])
    s.sendall(b"STATUS\n")
    data = bytearray()
    while b"\n" not in data:
        part = s.recv(1024)
        if not part:
            raise SystemExit("EOF: no retry; inspect receiver")
        data.extend(part)
        if len(data) > 4096:
            raise SystemExit("Oversized reply: stop")
    line, extra = bytes(data).split(b"\n", 1)
    if extra:
        raise SystemExit("Unexpected reply data: stop")
    print(time.monotonic_ns(), line.decode("ascii"))
PY
```

Require baseline IDLE, HEALTHY monitor, STOP/context/CD debt zero, no fault,
last_byte=NONE, last_gpio_tx=NONE, policy warn, warnings zero,
capture_integrity=NO_KNOWN_GAPS and receiver_integrity=HEALTHY. Verify socket
owner is the test operator and mode has no group/other access. Review startup
log and flags. A machine HEALTHY baseline does not establish receiver state.

Then, with **all remote buttons released**, run only the staged client:

```sh
umask 077
./cec-xs-client --socket "$XS8_CEC_TEST_DIR/control.sock" --max-hold-ms 500 \
  > "$XS8_CEC_TEST_DIR/cec.log" 2>&1
```

Confirm readiness and **zero service TX at startup**, without pressing anything.
The cap is deliberately 500 on both processes, not the template's 20000. Neither
process is autostarted or automatically restarted. Use another terminal for logs
and STATUS, not another control source.

## Ordered physical checks — explicit pauses, no automation

1. Authorize **one short DOWN tap/release only**, aimed below 250 ms; do not
   repeat to obtain a better result. Require client PRESS then RELEASE, service
   `17 E9 09 07`, separate STOP MATCH, ROOT MATCH, no cleanup retries or faults,
   and independently observed DOWN motion and cessation. A tiny/unobservable
   movement is inconclusive, not permission to repeat. Collect STATUS.
2. Pause and review. Only after independent approval and clean safety state,
   authorize one short UP tap/release. Require `17 F9 09 07` and independent UP
   movement/stop, with the same evidence and abort rules.
3. Pause. After separate approval, test one bounded DOWN hold: keep the physical
   button held about 700 ms and **release by 700 ms regardless of outcome**.
   This is not a guaranteed physical cutoff. Expected repeat PRESS messages do
   not create new STARTs; service/client cap requests STOP near 500 ms and no
   restart while still held. Client/service watchdog refusal may log uncertain
   PRESS_REPLY after service expiry; that specific refusal is expected only if
   cleanup succeeded and there is no waveform/fatal fault. Confirm release,
   physical cessation and clear debt. If the remote emits no repeats, record
   that limitation; do not claim continuous-repeat coverage. Do not simulate lost
   release by modifying hardware or leaving a button held indefinitely.
4. MUTE is separate: only when stationary and safe, explicitly approve one
   press/release. Expect `17 D8 07` once, LED changes flashing state, no motor
   movement. No automatic toggle back. A second MUTE to restore the initial
   LED state needs separate approval and readiness review, as in the
   [ON/OFF procedure](xs8-h11-mute-validation.md).

A PASS for the initial DOWN tap does not authorize later checklist items by
itself. Test each actual remote/TV path separately; Apple TV/onn/Panasonic
compatibility must not be inferred from another device's CEC event pattern.
Do not intentionally provoke adapter loss, client restart, service failure,
missing release or repeated UP holds on the receiver during this first session.
Those software paths are covered offline but still need separately reviewed
hardware validation before production approval.

Warnings alone are permitted only with otherwise valid waveform and physical
outcome: keep cumulative/per-TX counts, WARN_GLOBAL_GAP and receiver UNCERTAIN.
They must not be reset between actions to manufacture a clean result. Subsequent
readiness means safe IDLE with no debts/fault and consistent warning confidence,
not a demand to erase prior warnings.

## Abort, evidence and rollback

Cut receiver power immediately on unsafe/prolonged motion or unexpected START;
do not await the client timeout, max-hold timer or retry budget. Any non-MATCH,
fatal capture fault, STOP/ROOT failure or retry, unexpected TX/repeated MUTE,
panel lock, lost physical observation, unexplained missing release, RT/clock
failure or inconsistent confidence ends the session. Do not retry or inject
07/09/15/CD STOP as recovery. Preserve the fault and investigate independently.

Save UTC times, binary/config identity, full client/service logs and STATUS
before/after each action, event ages/durations/repeats, STOP reason/retry count,
all byte/tail/attribution evidence, RT timing and gap details. Record motor
movement/cessation and LED observations separately. Service receiver execution
must remain UNVERIFIED even when the operator reports a physical PASS.

After physical safety is established, Ctrl+C the foreground CEC client first,
wait for exit, inspect physical motor and service cleanup, collect STATUS, then
Ctrl+C the foreground service and retain logs. Orderly shutdown may attempt owed
cleanup; do not suppress it. Do not confuse process exit/GPIO LOW with STOP.
If uncertain, keep receiver power off. No production files changed, so rollback
of this session is simply ending both test processes; **do not automatically
restart the old GPIO bridge or production service**.

## Production deployment gate and minimal change plan

No application-code change is required for the attended test. Genuine remaining
release gates are installed-state comparison, attended CEC/repeat/hold validation,
reviewed production caps and restart behavior, and explicit unattended approval.
The repository template's 20 s cap is not approved by single-step physical tests.

After those gates, prepare a separate reviewed deployment diff against the actual
installed kenwood-xs8.service and client unit: point ExecStart at the checksummed
candidate under its separate name, select warn and approved RT priority only in
that service's environment, retain existing user/GPIO/socket permissions and
startup inhibition/authorization policy, and set explicitly approved hold caps
in both units. Keep the old binary and exact unit/drop-in/environment backups.
Do not use --allow-unrecovered-start on unattended boot without separately
reviewing physical baseline/restart risks; automatic recovery is not established.
Do not grant whole-application unrestricted RT privileges.

Exact production replacement/restart commands are **blocked pending the actual
unit and executable-path evidence**. Guessing an ExecStart, replacing a symlink,
or installing the repository 20 s template would bypass that gate. After a
reviewed deployment, rollback must stop the CEC frontend, establish physical
safety, stop the sole GPIO service, restore only the saved files/settings and
verify hashes/permissions before any separately approved restart. Retain strict
as the binary default; process warn opt-in is not a universal safety finding.
