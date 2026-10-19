# H11 Rev.C global-gap warning release candidate

Scope: an opt-in software candidate for attended, **receiver-disconnected**
fixture validation. This is not approval for unattended or receiver-connected
motor control. See [policy, attribution and limitations](xs8-global-gap-policy.md).

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
