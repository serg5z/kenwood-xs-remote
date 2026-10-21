# H11 Rev.C global-gap warning release candidate

Original build scope: an opt-in software candidate for attended,
**receiver-disconnected** fixture validation. Disconnected validation and one
subsequent receiver-connected TUNER and DOWN-step trials are recorded below.
These do not approve unattended operation or general motor control. See
[policy, attribution and limitations](xs8-global-gap-policy.md).

## Disconnected validation results — operator report

Reported during the 2026-10-19 readiness review for candidate `3a8ffecd`:

| Observation | Evidence level / interpretation |
|---|---|
| Receiver-disconnected strict and warn trials completed. | Operator-reported fixture execution; not receiver validation. |
| A real global-only sequence gap occurred during `SCHED_FIFO/5` TX. | Operator-reported live capture diagnostic; not merely synthetic fault injection. It does not establish a missing physical edge or its cause. |
| Warn mode retained `waveform=MATCH` and exposed `capture_integrity=WARN_GLOBAL_GAP`. | Operator-reported waveform and warning evidence remain separate. |
| Receiver integrity became `UNCERTAIN`. | Expected conservative confidence propagation, not a receiver malfunction report. |
| No fatal capture fault occurred during the successful warn trial. | Applies to that reported trial only; not proof of universal capture completeness. |

No raw log filenames, exact trial timestamps, transmitted byte, gap counters or
Pi-side checksum were supplied with this report. These details are not inferred
from the earlier H11 captures. The observations above are accepted as the
operator's report and have not been independently replayed or verified here.
The candidate's archived build checksum remains
`fc40aa188d65fe995788c59b4dc102d998dd3733de262fe2249f3e1fa403aaf2`;
the operator must verify the test binary against it before another trial.

These results support proceeding to a **separate readiness review**, not
unattended operation or motor testing. See the
[single-command receiver-connected proposal](xs8-h11-connected-readiness.md)
for prerequisites, isolation limits, stop conditions and rollback. No connected
test was performed by the documentation reviewer. A subsequent operator-reported
connected trial is recorded in the next section.

## Receiver-connected TUNER result — operator report

Confirmed results supplied during the 2026-10-19 review of candidate `3a8ffecd`:

| Evidence | Confirmed result | Boundary |
|---|---|---|
| Experiment | One attended `SELECT_SOURCE TUNER`, logical TX `0x82`. | Receiver-connected input-selection trial, not a motor trial. |
| Independent physical observation | Kenwood switched **PHONO → TUNER**. | Operator-observed selection, distinct from software waveform verification. |
| Independent physical observation | Volume motor remained stationary. | No motor START/STOP was tested. |
| Software TX evidence | `gpio_tx=TX_OK`, `waveform=MATCH`. | Transmission and matching observed waveform, not an execution acknowledgment. |
| Software capture evidence | One global-only sequence gap; `monitor=HEALTHY`, `capture_integrity=WARN_GLOBAL_GAP`. | Operational health and reduced capture confidence remained distinct. |
| Service receiver evidence | `receiver_integrity=UNCERTAIN`, `receiver_execution=UNVERIFIED`. | Correctly does not incorporate the human observation as machine-verified execution. |
| Operator-reported TX scope | No retry or additional TX was performed. | Does not imply there was no unsolicited RX traffic. |

This is a physical PASS for that input-selection trial, accompanied by reduced
capture confidence. It does not establish the cause of the gap, validate STOP
delivery, or demonstrate universal receiver safety. Raw log paths, exact event
counters/timestamps and a per-trial Pi checksum were not supplied with this
summary; no corresponding archived trace is asserted here.

The next-stage [volume START/STOP safety review](xs8-h11-volume-safety-review.md)
now records **PASS for one attended DOWN step after operator preflight**,
following the offline START/STOP fault-injection tests and the specified physical
cutoff setup. This does not approve held controls, MUTE, production restoration
or unattended use. Recording these results does not change production settings
or the compiled strict default.

## Receiver-connected DOWN step — operator report

Reported during the 2026-10-19 UP readiness review for candidate `3a8ffecd`:

| Evidence | Reported result | Boundary |
|---|---|---|
| Independent physical observation | Motor moved DOWN and independently ceased moving. | Physical PASS for this one attended step; not inferred from TX evidence. |
| Transmitted sequence | Four expected logical frames: `0x17 → 0xE9 → 0x09 → 0x07`. | STOP and ROOT completed without retries. |
| Capture diagnostics | Five global-only warnings; no fatal monitor fault. | Does not establish why sequence numbers skipped or complete physical-edge capture. |
| Receiver confidence | `receiver_integrity=UNCERTAIN` correctly remained set. | Human observations do not create machine receiver feedback. |

The report supplies no raw log path, exact trial timestamp, per-frame warning
allocation or measured physical runtime; none are invented here. STOP execution
was operator-observed, not acknowledged by the receiver protocol. This result
supports reviewing the [single attended UP procedure](xs8-h11-volume-up-validation.md),
not repeating commands automatically, held control or production restoration.

## UP completion and next MUTE review — operator report

During the 2026-10-19 MUTE review, the operator confirmed that the attended
TUNER, Volume DOWN and Volume UP trials passed physical observation. This adds
the UP physical PASS; no raw UP log, warning count or exact timing was supplied
in this update. The operator subsequently confirmed that the red volume-knob LED flashes for
MUTE ON and does not flash for OFF, with speakers disconnected. The
[two-stage attended MUTE proposal](xs8-h11-mute-validation.md) now uses that
physical indication, a fresh baseline and separate ON/OFF confirmations. No H11
MUTE physical result is claimed yet.

## Later operator reports — reconciliation recorded 2026-10-21

The preceding October 19 review sections retain what was known at those review
boundaries. Subsequently, the operator confirmed attended MUTE ON and OFF:
the red volume-knob LED began flashing after the first MUTE and stopped flashing
after the separately confirmed second MUTE. Speakers remained disconnected.
TUNER, Volume DOWN, Volume UP and both MUTE stages were reported TX_OK/MATCH;
global-only warnings left monitor health HEALTHY and receiver integrity UNCERTAIN.
These physical observations are independent of waveform verification. Exact
trial timestamps and raw trial logs were not supplied locally.

Later attended CEC integration physically moved and stopped volume UP/DOWN,
but eventually latched a fault after a STOP returned TX_OK/INVALID. Subsequent
STOP and ROOT attempts MATCHed; the retry helper intentionally retained the
prior failure. Execution of the particular problematic STOP was not independently
observed. The operator reported both experimental services stopped and logs
preserved on the Pi under `~/kenwood-build-review/logs/h11-cec-20261019/`.
Those logs are not assumed to be in this repository. No conclusion that the
failed STOP was correctly executed follows from later matching frames.

These updates supersede the earlier pending-MUTE status, without converting
attended trials into unattended or held-button safety validation. **Unattended
production restoration remains BLOCKED** pending resolution of uncertain STOP
observation and separately authorized validation. No retry or fault policy is
changed by these records.

## Reviewed execution path

1. `xs8_service.cpp` opens the exclusive GPIO requests, then starts the manual
   monitor. An unset policy selects `strict`; an invalid value aborts startup.
   GPIO initialization drives TX inactive but sends no XS8 frame. Default
   startup remains BOOT_INHIBIT; no automatic recovery is added.
2. One service control thread services sockets, checks interlocks, transmits
   synchronously, drains RX and verifies the observation. No socket handler or
   monitor worker gains another TX owner. Monitor failure blocks new commands;
   existing owed STOP/RELEASE/context cleanup attempts retain their safety path.
3. Global and per-line counters are checked independently. Only a forward
   global-only discontinuity can warn. Line gaps/regressions, global regressions,
   timestamp/read faults, unexpected lines and detected overflow stay fatal.
4. Local verification drains ready observations and requires one candidate in
   the recorded TX window, matching its byte and strict DATA-before-BUSY tail.
   Ambiguous, malformed or additional unfinished candidates do not MATCH.
   Post-window frames remain separate observations, never acknowledgments.
5. STATUS distinguishes operational health, cumulative capture warnings,
   per-attempt warnings and waveform evidence. The Gateway accepts the validated
   optional diagnostic group without weakening authentication or command gates.
6. Warning-contaminated receiver observations cannot establish state. Last-known
   state becomes uncertain; a clean explicit query can reconcile it. No query,
   retry, recovery or policy change is automatically introduced.

The H11 target requires BCM DATA_TX=6, BUSY_TX=19, DATA_RX=13 and BUSY_RX=26.
The candidate must not be built from a source snapshot retaining the old pins.
Timing constants, RT scheduling and command sequences are unchanged.

## Identity and artifacts

The release directory contains:

- `xs8-service-h11-warn`: ARMv6 hard-float candidate; a separate name.
- `SHA256SUMS-h11-warn`: checksum for that exact file.
- `manifest.json`: source commit/tree, build image, compiler flags and checksum.
- `validation.txt` and `packages.txt`: ELF, loader, dependency and build-package
  checks from the Raspberry Pi OS-compatible ARMv6 container.

The binary is built from the reviewed Git tree, not the rest of the dirty
working directory. Artifacts remain ignored under `.build/`; none are committed.
The existing `xs8-service-h11` file is never an output destination. The candidate
has no embedded version command: identity is established by checksum and the
manifest. Do not invoke it merely to inspect its version; it opens GPIO on startup.

On the Mac, in the candidate artifact directory:

```sh
shasum -a 256 -c SHA256SUMS-h11-warn
cat manifest.json
```

After a separately approved manual transfer into a new Pi staging directory:

```sh
sha256sum -c SHA256SUMS-h11-warn
file xs8-service-h11-warn
readelf -h -A -l -d xs8-service-h11-warn
```

Compare the manifest source commit with `git show --stat <commit>` and verify
ARMv6, hard-float ABI and `/lib/ld-linux-armhf.so.3`. Dependency checks use the
container's matching libraries; installed Pi library compatibility still needs
checking. These inspection commands do not execute the service.

## Proposed disconnected validation (operator-run only)

1. With equipment powered off, physically disconnect the Kenwood and any CD
   equipment from the H11 fixture. Verify the Rev.C wiring and common ground.
   Do not reconnect while any diagnostic process is running.
2. The operator must arrange exclusive ownership: `kenwood-xs8.service` and all
   other GPIO tools must be inactive. Verify that state manually; do not run
   alongside `xs8_sniff`, which requests the same RX lines. Do not edit service
   units, symlinks, environment files or production binaries.
3. In the new staging directory, first run a foreground strict baseline using
   an isolated socket (no production CEC/Web clients):

   ```sh
   XS8_GLOBAL_GAP_POLICY=strict ./xs8-service-h11-warn \
     --socket /tmp/kenwood-xs8-h11-warn-test.sock \
     --allow-unrecovered-start --enable-research-tx --max-hold-ms 500 \
     > strict-fixture.log 2>&1
   ```

   This authorization acknowledges the disconnected, externally checked fixture;
   it is not recovery or evidence of receiver state. Use the existing GPIO
   permissions. Do not grant new RT privileges; record the existing
   `XS8_RT_PRIORITY` environment and keep it identical across comparisons.
4. From another terminal, connect the repository REPL to that isolated socket:

   ```sh
   python3 xs8_repl.py --socket /tmp/kenwood-xs8-h11-warn-test.sock
   ```

   Run `status`, then one `send 0x84`, then `status` and `quit`. No chains,
   motor commands or scans. Record the requested byte, GPIO TX result, waveform,
   full STATUS and warning/fatal diagnostics. Receiver state is not expected to
   become known in a disconnected fixture.
5. Stop the foreground candidate with Ctrl+C and retain the complete log.
   Only after it exits, repeat the same single-frame trial with the startup
   environment changed explicitly to `XS8_GLOBAL_GAP_POLICY=warn` and log output
   `warn-fixture.log`. Policy is fixed per process. No persistent setting changes.
6. For a global-only warning, require matching byte/tail evidence and separately
   visible warning counts/confidence. A fatal per-line/timestamp/read fault, an
   invalid/missing/ambiguous waveform or unexpected output ends the trial.
   Do not retry automatically or reconnect the receiver to continue testing.
7. Compare the runs without claiming that absence of a gap proves its cause.
   Preserve full logs, manifests and any independently obtained captures.
   Further repetitions or RT comparisons require a separately bounded plan.

## Rollback

Stop the foreground candidate, verify it exited and released GPIO, and retain
its logs. No production file, symlink, unit or environment setting was replaced,
so there is no binary rollback operation. Do not copy the candidate over
`xs8-service-h11`. Restoring the previously approved production service, reconnecting
equipment or authorizing receiver control are separate operator actions; this
procedure performs none of them. The compiled default remains strict even for
this candidate; only its per-process opt-in selects warn.

## Release limits

- Source review and synthetic tests do not establish the kernel cause of gaps.
- Dense per-line counters and MATCH do not prove physical edge completeness or
  receiver execution. Delayed IRQ delivery can reveal a fault after a reply.
- TX completion includes software overhead; a response starting inside that
  window cannot be assigned an electrical origin and is conservatively ambiguous.
- Passive decoding still permits archived tail conventions and ignores DATA
  activity outside an active frame; local verification is deliberately stricter.
- Software cannot guarantee STOP/RELEASE after process death, power loss, wiring
  failure or an unexecuted command. Existing watchdogs do not add feedback.
