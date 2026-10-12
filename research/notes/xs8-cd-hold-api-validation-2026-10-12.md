# CD Transport Hold API: physical validation, 2026-10-12

This report records the operator-supplied successful hardware validation of the
[CD transport socket API](../../docs/cd-transport-api.md). No hardware commands,
deployment or code changes were performed while documenting these results.
The report date is 2026-10-12; individual trial timestamps, raw captures,
complete socket transcripts and a deployed binary checksum were not supplied.

## Hardware and build provenance

- Raspberry Pi B+ Rev 1.2, ARMv6 (`armv6l`).
- Kenwood KR-V6070 with connected CD changer; changer model not supplied.
- `xs8-service` cross-built on macOS using the Docker/QEMU ARMv6 workflow.

The operator reports that the Docker-built service executed successfully on the
real Pi and performed the operations below. This adds actual target execution
and physical behavior evidence to the earlier [container ELF validation](../../docs/pi-build.md#validation-recorded-on-2026-10-12).
The exact deployed artifact, runtime package versions and service startup
options cannot be independently identified from this report. It does not
validate every cross-built executable or CEC integration.

## Reported successful operations

All transmitted bytes below use **current logical TX**, directly in ROOT.
The sequences are the existing API mapping, supported by the earlier
[physical KEY RELEASE research](xs8-cd-key-release-2026-10-12.md); they are not
new RX decoding results. No entered-context experiment is reported.

| API operation | Existing logical TX / socket behavior | Operator-reported result and evidence boundary |
|---|---|---|
| `CD_TRACK_NEXT` | `0xFC → 0x72`, nominal 100 ms TX-end-to-next-TX-start gap | **Physical PASS:** one track forward in the tested playback state. No measured gap was supplied for this API trial. |
| `CD_TRACK_PREVIOUS` | `0xEC → 0x72`, same discrete gap policy | **Physical PASS:** restarted the current track. No automatic second press was needed or asserted; this does not establish a universal previous-track result. |
| `CD_PRESS` followed by explicit `CD_RELEASE <token>` | KEY DOWN once, then `0x72` KEY UP | **Reported PASS:** successful KEY DOWN / KEY UP. The action for this general trial was not specified; the separate forward-seek trial is explicit below. |
| Lease inactivity timeout | Service automatically attempts logical `0x72` | **Reported PASS:** automatic RELEASE on expiry. The supplied account identifies the release, but contains no separate expiry-trial physical-motion description or raw waveform record. |
| Fresh PRESS after expired lease | A new accepted hold receives a new token | **Reported API PASS:** fresh PRESS accepted with a new token. Expiry did not permanently inhibit CD control after cleanup. Token values and full replies were not supplied. |
| Six `CD_HEARTBEAT <token>` renewals | Renew lease without transmitting another KEY DOWN | **Reported API PASS:** six renewals over approximately 3 seconds. This validates the reported lease-renewal flow, not indefinite physical operation or behavior under arbitrary network delay. |
| `CD_PRESS SEEK_FORWARD` followed by explicit RELEASE | `0x7A`, then logical `0x72` | **Physical PASS:** forward seek stopped physically after explicit RELEASE. This physical observation is separate from waveform matching. |

The API's nominal 100 ms discrete gap is an implementation setting, not a
measured timing result from this validation. The approximate 3-second renewal
interval is operator-reported; exact heartbeat spacing, timeout latency and
frame timing statistics were not supplied.

## Final service status supplied by the operator

The following are the supplied fields, preserved individually rather than
presented as an invented complete wire reply:

```text
state=IDLE
monitor=HEALTHY
cd_state=IDLE
cd_release_owed=0
cd_lease_expired=0
cd_last_result=CLIENT_RELEASE
last_byte=0x72
last_waveform=MATCH
receiver_integrity=HEALTHY
```

These fields establish the reported **software state and monitor evidence**:
the service retained no CD RELEASE obligation, its final observed waveform
matched `0x72`, and monitor/receiver-observation integrity was reported healthy.
`cd_lease_expired=0` is consistent with accepting a subsequent fresh operation;
it does not erase the earlier expiry observation. This is final status, not a
per-trial status record or a reconstructed chronology of every request.

## Evidence levels and remaining limitations

1. **GPIO transmission completed:** `TX_OK` reports a completed software/GPIO
   transmission attempt. Per-frame TX replies were not supplied here.
2. **Matching bus waveform observed:** the supplied final
   `last_byte=0x72` / `last_waveform=MATCH` reports electrical observation of a
   matching RELEASE waveform. It is not a receiver acknowledgment.
3. **Physical execution observed:** one-track NEXT, current-track restart and
   forward SEEK termination were independently reported by the operator.
   Those observations, rather than service status alone, support the physical
   PASS findings.

The supplied results are not independently replayable from archived raw edge
data, a binary checksum or video. No universal one-track timing guarantee,
unlimited-duration physical endurance test, or compatibility with another
receiver/changer has been established. This API validation does not newly
establish held SEEK_BACKWARD or held TRACK_PREVIOUS behavior; earlier protocol
trials remain separate evidence.

Failed/malformed PRESS or RELEASE, missing monitor events, monitor failure,
fault recovery, service shutdown during a hold, process crash, power loss,
lost ground and unstable GPIO were not physically exercised in this report.
Existing offline failure-path tests establish software obligations, not
hardware termination. Software still cannot guarantee KEY UP after termination
or hardware failure, and a matching waveform does not prove physical execution.

There is no new evidence for CD RELEASE in the `0x17` context or for a universal
XS8 key-up. Receiver-context research entry/RUN restrictions remain unchanged.

## Follow-up evidence to preserve

For future attended trials, archive the deployed service checksum/build
manifest, effective options, timestamped request/reply and monitor logs, and
independent physical results. Keep token values private. Record actual discrete
gaps and lease-expiry latency before making timing guarantees. Any further
physical testing needs its own attended recovery plan; this report authorizes
no automatic experiment or deployment.
