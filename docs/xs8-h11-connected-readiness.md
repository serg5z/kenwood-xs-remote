# H11 Rev.C: single-command receiver-connected readiness

> Historical review/procedure retained from the October 19 investigation.
> Reconciliation recorded 2026-10-21: see the [later operator reports](xs8-h11-warn-rc.md#later-operator-reports--reconciliation-recorded-2026-10-21),
> including successful attended trials and the subsequent uncertain STOP/fault.
> An earlier conditional PASS below is not renewed authorization after that fault.

## Verdict and scope

**PASS for the procedure design: one attended direct-socket `SELECT_SOURCE TUNER`
trial, conditional on every prerequisite below. Execution is BLOCKED if any
prerequisite cannot be established.** This does not authorize unattended use,
volume/CD testing, a general Web trial or the permanent adoption of warn mode.

Candidate: `3a8ffecd985215a8a748d2bb788847756337e6f5`, separate binary
`xs8-service-h11-warn`, SHA-256:

```text
fc40aa188d65fe995788c59b4dc102d998dd3733de262fe2249f3e1fa403aaf2
```

The [disconnected results](xs8-h11-warn-rc.md#disconnected-validation-results--operator-report)
are operator-reported evidence. They support testing the warning path against a
receiver, but do not demonstrate receiver acceptance or electrical completeness.
No hardware or running service was accessed for this review.

## Reviewed command and safety paths

| Path | Finding |
|---|---|
| Startup | Without authorization, BOOT_INHIBIT remains. `--allow-unrecovered-start` moves the software to IDLE without recovery TX; it is an operator assertion, not proof of receiver ROOT. [Startup](../volume_control.cpp#L78-L84), [service options](../xs8_service.cpp#L275-L334). |
| Single source | `SELECT_SOURCE TUNER` maps to **logical TX `0x82`**, legacy TX `0xBE`. TUNER selection has prior physical evidence. It is not a passive RX byte. [Evidence](source-selection.md), [direct service branch](../xs8_service_core.cpp#L640-L663). |
| Interlocks | Active CD work/faults and unhealthy monitor reject new source commands; the source branch additionally requires IDLE, no STOP owed and no open context. [Dispatch](../xs8_service_core.cpp#L565-L607), [source checks](../xs8_service_core.cpp#L655-L657). |
| TX and verification | One `send(0x82)` through the existing owner, BUSY-free checks, RT wrapper and monitor verifier. No source retry, context entry or release sequence is added. [Transmitter](../xs8_service.cpp#L95-L140). |
| Source result | `OK SOURCE_TX ... waveform=MATCH` reports a matching local waveform, never physical selection. The source cache is cleared after attempted TX; no automatic QUERY_STATUS follows. [Cache handling](../xs8_service.cpp#L1194-L1212). |
| Gateway difference | The Gateway may append CD_STOP when leaving a freshly observed CD source. **Do not use the Web UI, Gateway, CEC client or REPL for this trial.** The direct source branch has no such action. [Gateway path](../xs8_gateway.py#L1314-L1324). |
| Observation faults | Warn exempts only forward global-only gaps. Fatal faults still block new commands; byte and strict local-tail checks remain required. [Policy and limits](xs8-global-gap-policy.md). |
| Disconnect/shutdown | A fresh IDLE service with no hold, STOP/RELEASE obligation or context has nothing to terminate. Cleanup can send termination commands if an obligation exists; it must not be described as unconditionally TX-free. [Disconnect/tick/shutdown](../xs8_service_core.cpp#L824-L909), [volume shutdown](../volume_control.cpp#L337-L345). |

### What prevents unintended commands

The hardcoded client below sends only STATUS and **one** SELECT_SOURCE TUNER.
With the checked fresh-IDLE baseline and no competing clients, that path cannot
dispatch CD transport, volume START, a research chain or `0x17` context entry.
The launch omits all research and CD-probe flags. Startup and ordinary source
completion do not synthesize commands. Watchdogs have no active hold to end.

**This is procedural isolation, not a TUNER-only service mode.** The candidate
still accepts other production commands from an authorized local socket client,
including volume/CD commands and production context keys. Omitting research
flags does not disable those APIs. A private socket excludes accidental clients
using the normal production path and other unprivileged users; it does not
exclude root or another process running as its owner. If that trust boundary
cannot be assured, the trial is BLOCKED; a future restricted test mode would be
needed. Do not weaken cleanup or other safety interlocks to obtain isolation.

## Prerequisites — operator must verify before launch

1. Verify the staged binary checksum above and manifest commit. Do not replace
   `xs8-service-h11`, production symlinks, units, credentials or environment files.
2. `kenwood-xs8.service`, the fallback GPIO bridge and every GPIO diagnostic must
   already be inactive. Confirm no other owner of GPIO6/19/13/26. If a service
   is active, stop this procedure and arrange a separately approved maintenance
   window; no stop/restart command is part of this plan.
3. With equipment powered off, inspect Rev.C wiring, supply and common ground;
   connect H11 to the KR-V6070 only. Disconnect the CD changer and other XS8
   peripherals to remove additional masters and transport interactions. Keep the
   changer powered off; if it uses the receiver's switched outlet, disconnect it
   with all equipment powered off before this trial. No hot rewiring. Establish
   the receiver's normal operating baseline independently.
4. Manually set volume to minimum, silence sources, and select **PHONO** at the
   front panel. Confirm the front panel works and the volume motor is stationary.
   No prior unresolved hold, front-panel lock, context experiment or uncertain
   recovery may remain. Service restart, `context_may_be_open=0`, initial
   `receiver_integrity=HEALTHY` and even a working front panel do not establish
   the receiver's hidden XS8 context. If ROOT baseline is uncertain, BLOCKED:
   establish it through a separately justified recovery procedure, not `0x07`,
   `0x09`, a speculative query or a claim that power cycling must reset context.
5. One operator must watch the receiver throughout; its physical power control
   and an independent receiver power disconnect must remain reachable. No remote
   presses, browser actions or front-panel
   changes during the actual TX. Prevent other processes/users from targeting
   the test socket; verify CEC/Gateway clients retain their normal socket path.
6. Use the exact launch identity and existing RT privileges that produced the
   disconnected `SCHED_FIFO/5` trial. Do not add capabilities, use whole-process
   `chrt`, run under a new privilege model or change scheduling configuration.
   If this cannot be reproduced without changes, stop and review first.

## Operator-run procedure — not executed here

From the separately staged candidate directory, verify `sha256sum -c
SHA256SUMS-h11-warn` and the manifest first. In terminal A, create a fresh private
directory; retain its printed path for terminal B:

```sh
umask 077
XS8_VALIDATION_DIR=$(mktemp -d /tmp/xs8-connected.XXXXXX)
chmod 700 "$XS8_VALIDATION_DIR"
printf 'Test directory: %s\n' "$XS8_VALIDATION_DIR"
XS8_GLOBAL_GAP_POLICY=warn XS8_RT_PRIORITY=5 ./xs8-service-h11-warn \
  --socket "$XS8_VALIDATION_DIR/control.sock" \
  --allow-unrecovered-start --max-hold-ms 500 \
  > "$XS8_VALIDATION_DIR/service.log" 2>&1
```

Use existing approved privileges; the environment above is per-process only.
The 500 ms volume bound is a backstop, **not** permission to send motor commands
and not a CD hold limit. No research, chain, context or CD-probe flag is present.

Before terminal B proceeds, inspect startup logs and directory/socket ownership
and permissions. Require all four feature flags DISABLED and no transmitted
frame. A startup failure or unexpected activity ends the trial. Optionally view
the log read-only with `tail`; do not start a second GPIO monitor.

In terminal B set `XS8_VALIDATION_DIR` to the exact printed directory, then run
this fixed client once. It has no retry, arbitrary-command or recovery path.
It validates readiness immediately before its only state-changing request.

```sh
python3 - "$XS8_VALIDATION_DIR/control.sock" <<'PY'
import socket
import sys

path = sys.argv[1]

def request(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(3)
        client.connect(path)
        client.sendall(command)
        response = bytearray()
        while b"\n" not in response:
            chunk = client.recv(1024)
            if not chunk:
                raise RuntimeError("Disconnected; outcome uncertain; do not retry")
            response.extend(chunk)
            if len(response) > 4096:
                raise RuntimeError("Oversized reply; do not retry")
        line, remainder = bytes(response).split(b"\n", 1)
        if remainder:
            raise RuntimeError("Unexpected extra reply; do not retry")
        text = line.decode("ascii")
        print(text, flush=True)
        return text

if input("Prerequisites checked; type ONE_TUNER to proceed: ") != "ONE_TUNER":
    raise SystemExit("Cancelled; no request sent")
before = request(b"STATUS\n").split()
if not before or before[0] != "OK":
    raise SystemExit("STATUS rejected; no selection sent")
fields = {}
for item in before[1:]:
    key, value = item.split("=", 1)
    if key in fields:
        raise SystemExit("Duplicate STATUS field; no selection sent")
    fields[key] = value
required = {"state": "IDLE", "monitor": "HEALTHY", "stop_owed": "0",
            "context_may_be_open": "0", "cd_state": "IDLE",
            "cd_release_owed": "0", "cd_lease_expired": "0",
            "last_byte": "NONE", "last_gpio_tx": "NONE",
            "receiver_integrity": "HEALTHY", "global_gap_policy": "warn",
            "global_gap_warnings": "0", "capture_integrity": "NO_KNOWN_GAPS"}
if any(fields.get(key) != value for key, value in required.items()):
    raise SystemExit("Baseline not clean; no selection sent; review without retry")
reply = request(b"SELECT_SOURCE TUNER\n")
expected = ("OK SOURCE_TX gpio_tx=TX_OK waveform=MATCH "
            "receiver_execution=UNVERIFIED")
if reply != expected:
    raise SystemExit("Selection uncertain/rejected; no retry; check receiver and log")
request(b"STATUS\n")
input("Observe source display and motor; press Enter to record final STATUS: ")
request(b"STATUS\n")
PY
```

Retain terminal B output alongside `service.log`; do not rerun the client after
an uncertain outcome or to force a global gap. The final STATUS can reveal a
later-delivered fault, but no finite observation interval guarantees that all
events have arrived. Check the log again before declaring the trial complete.

## Expected evidence and success criteria

- Exactly one intended TX, logical `0x82`, with `rt=FIFO` and timing statistics.
  Retain the launch command specifying priority 5; the setting plus RT result
  documents the requested/accepted policy, not an independent scheduler measurement.
  If RT could not be enabled, preserve the explicit failure log and classify
  the FIFO/5 trial as inconclusive; do not retry or change privileges.
- The source reply reports TX_OK/MATCH. This is waveform evidence only.
- Receiver front-panel display physically changes PHONO → TUNER; operator
  records it independently. Volume knob/motor stays stationary, and front-panel
  controls remain usable. Check front-panel usability after TX, not during it.
- No intended `0x17`, motor START, STOP/ROOT recovery, CD key, chain or query TX.
  RX observations are separate multi-master traffic, not acknowledgments.
- Final software state is IDLE, no STOP/context/CD release owed, monitor HEALTHY.
  If a global-only warning occurs, warning counters/confidence must expose it
  and receiver integrity must become UNCERTAIN even if the display is TUNER.
  Cached source may remain UNKNOWN: no readback is requested or fabricated.
- A trial without a gap can validate this physical input change but does not
  re-exercise warn handling. Do not manufacture a gap or repeat automatically.

## Stop conditions and rollback

Abort on unexpected motion, input/transport action, panel lock, unexpected TX,
fatal capture fault, non-MATCH, timeout/disconnection, contradictory evidence or
loss of operator attention. No retries, automatic readback, context-exit frame,
software reset or motor command is an acceptable recovery step for this trial.

For unexpected motor motion or an unsafe physical condition, use the receiver's
physical power control immediately, using the independent power disconnect if
the panel is unresponsive. Ctrl+C, a timeout or a watchdog is not a guaranteed
motor stop. For an ordinary completed or rejected input trial, stop
only the foreground candidate with Ctrl+C, retain output and verify process exit
and GPIO release. Cleanup could transmit if an unexpected obligation exists;
record it and treat the single-command trial as failed, never suppress cleanup.

No production binary, symlink, unit or environment file was changed, so rollback
does not copy files or start services. Leave production inactive until separately
approved. Restore receiver settings manually only after the candidate exits;
power down before any rewiring. Preserve logs/checksum and record physical
observations, exact time, RT outcome, warning/fatal counters and receiver state.

Remaining risks include corrupted/missing electrical edges before numbering,
coarse TX completion attribution, delayed faults, hidden receiver context,
unattributed external traffic, same-UID/root socket access and wiring/power loss.
Neither this plan nor the warn policy supplies receiver acknowledgments or
guarantees motor shutdown after hardware/process failure.
