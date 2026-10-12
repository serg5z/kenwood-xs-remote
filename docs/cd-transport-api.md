# CD transport socket API, phase 2

`xs8-service` remains the only XS8 GPIO owner. Commands use its **local Unix socket** and ROOT context. The [protocol evidence](xs8-protocol.md#cd-transport-press-and-release-in-root) is specific to the KR-V6070 and connected changer. This API does not add a Web endpoint or CD STOP command.

| Request line | Logical ROOT frame(s) | Meaning in the tested setup |
|---|---|---|
| `CD_TRACK_NEXT\n` | logical `0xFC → 0x72` | One discrete NEXT press/release attempt. |
| `CD_TRACK_PREVIOUS\n` | logical `0xEC → 0x72` | One discrete PREVIOUS press/release attempt; it may restart the current track. The service never sends a second pair automatically. |
| `CD_PRESS TRACK_NEXT\n` | `0xFC` | Begin a held NEXT operation. |
| `CD_PRESS TRACK_PREVIOUS\n` | `0xEC` | Begin a held PREVIOUS operation. |
| `CD_PRESS SEEK_FORWARD\n` | `0x7A` | Begin a held forward seek. |
| `CD_PRESS SEEK_BACKWARD\n` | `0x6A` | Begin a held backward seek. |
| `CD_HEARTBEAT <token>\n` | None | Renew the active hold's lease. |
| `CD_RELEASE <token>\n` | `0x72` | End the active held operation. |

The discrete commands complete both frame attempts before replying. After a verified PRESS, they request a **100 ms minimum gap from the recorded end of KEY DOWN TX to the beginning of KEY UP TX**. Waveform verification, BUSY-free checks, and scheduling can make the actual gap longer. If PRESS transmission or waveform verification is uncertain, RELEASE is attempted immediately. The explicit `CD_PRESS` commands send KEY DOWN **once** and hold while their lease is renewed. They do not use the 100 ms gap or repeat KEY DOWN. No `0x17` context entry or `0x9C` CD STOP is substituted.

On a fully matched pair, a reply has this form:

```text
OK CD_TRACK_TX action=NEXT press_gpio_tx=TX_OK press_waveform=MATCH release_gpio_tx=TX_OK release_waveform=MATCH cd_result=TX_VERIFIED receiver_execution=UNVERIFIED
```

`PREVIOUS` uses `action=PREVIOUS`. `TX_VERIFIED` means both GPIO transmissions completed and their bus waveforms matched; it is **not** an acknowledgment from the receiver or a guaranteed track movement. An uncertain pair returns `ERR CD_TRACK_UNCERTAIN` with the same evidence fields and `cd_result=PRESS_UNCERTAIN` or `RELEASE_UNCERTAIN`. A failed PRESS still triggers one RELEASE attempt. A failed or missing RELEASE match latches `cd_state=FAULT_LATCHED` and retains `cd_release_owed=1` until a confirmed transmission or process restart. A PRESS failure also latches the CD fault even when RELEASE matches, because the operation's outcome remains uncertain.

An explicit verified press replies `OK CD_PRESS_TX action=TRACK_NEXT hold_token=1 gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED`, where the decimal token is an example. The client sends `CD_HEARTBEAT 1` about every **500 ms** and receives `OK CD_LEASE_RENEWED`; this transmits no XS8 frame. It sends `CD_RELEASE 1` for immediate KEY UP and receives `OK CD_RELEASE_TX reason=CLIENT_RELEASE gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED` when the waveform matches. An explicit hold has **no absolute maximum duration** while valid heartbeats continue. Uncertain results reply `ERR CD_PRESS_UNCERTAIN` or `ERR CD_RELEASE_UNCERTAIN` with available TX/waveform fields. A duplicate press while held returns `ERR CD_TRACK_BUSY`; a release with no active hold returns `ERR CD_NOT_ACTIVE` and sends no frame. A missing/malformed token returns `ERR CD_TOKEN_REQUIRED`; a stale or wrong token returns `ERR CD_NOT_OWNER`. An uncertain PRESS triggers immediate RELEASE and latches a fault. A failed RELEASE retains its obligation and latches a fault. No automatic retry of KEY DOWN occurs.

`STATUS` includes `cd_state=IDLE|ACTIVE|HOLDING|FAULT_LATCHED`, `cd_action`, `cd_release_owed=0|1`, `cd_lease_expired=0|1`, and `cd_last_result`. It does not expose the hold token. A CD fault also reports the existing overall `state=FAULT_LATCHED` so Gateway readiness cannot mistake it for ordinary IDLE. Other status fields remain. New CD requests require a local client, healthy monitor, receiver control `IDLE`, no volume STOP obligation, and no open `0x17` context. Shutdown and an earlier uncertain research TX also block new CD presses. A CD hold blocks conflicting active commands, including CEC volume START, MUTE, source selection, queries, and research TX; they receive prompt error replies. STATUS and token-matched HEARTBEAT/RELEASE remain available from other connections. A research chain already in progress rejects CD commands under its existing session-busy rule. The single owner thread sends no unrelated frame between KEY DOWN and KEY UP.

The service accepts up to **eight simultaneous local socket clients** and still runs every GPIO command on one control thread. A CD hold belongs to its service-owned token, not to the connection that requested PRESS. That connection may close; a short-lived request from another client can renew or release the hold with the token. Each hold receives an independently generated, nonzero 64-bit token before KEY DOWN; token-generation failure rejects PRESS without transmission. Keep this token private: possession permits renewal or release over any authorized local socket connection. STATUS and logs do not expose it. Local socket permissions and future Gateway authorization remain necessary. The persistent CEC client and short-lived Gateway requests can receive STATUS or prompt busy responses throughout a CD hold. A future Web control API must bind the token to an authenticated browser session and send bounded heartbeats; no Web endpoint is implemented here. The existing discrete commands are suitable for short taps.

## Failure, shutdown, and operator recovery

The release obligation is set **before** KEY DOWN may reach the bus. The service attempts `0x72` after a failed or uncertain PRESS and after an interrupted discrete wait. For explicit holds it also attempts RELEASE on token-matched client request, monitor failure, **2000 ms without a valid heartbeat**, and orderly shutdown. Socket disconnect alone does not release a CD hold; the lease timeout covers a lost short-lived client. The heartbeat and lease clocks are monotonic. The control loop checks expiry approximately every 20 ms when otherwise idle. After a matched expiry RELEASE, a fresh CD PRESS or discrete tap is permitted subject to all existing preflight checks. `cd_lease_expired=1` and `cd_last_result=LEASE_EXPIRED` describe the completed lease until a new CD operation is accepted; they are not a fault or a restart requirement. An accepted operation clears the expiration flag and a new hold receives a fresh token. HEARTBEAT/RELEASE with an expired token returns `CD_NOT_ACTIVE` while idle or `CD_NOT_OWNER` during a new hold, without transmission or renewal. An uncertain RELEASE still latches a CD fault and retains its obligation. Other CEC and Web operations can resume after a matched release, subject to their existing safeguards. Monitor failure during a hold also latches a fault. If the first RELEASE is unconfirmed, a graceful shutdown makes at most one additional RELEASE attempt. Failure leaves a visible fault and a prominent log warning. The service does **not** automatically send `0x9C`, repeat the track command, or interpret `TX_OK`/`MATCH` as physical execution.

`SIGINT`/`SIGTERM` request shutdown through the normal control thread; a signal during the pair does not intentionally skip RELEASE. Process crash, `SIGKILL`, power loss, failed GPIO hardware, lost ground, or a receiver that ignores a correctly observed waveform can still leave transport motion active. Software cannot guarantee physical termination in those cases.

After any `CD_FAULT_LATCHED`, the operator must **physically stop and inspect the changer**, restore a known receiver/CD state, and inspect the service's release evidence before restarting. The fault is deliberately not clearable over the socket. A restart begins in `BOOT_INHIBIT`; leaving it requires the existing explicit `--allow-unrecovered-start` startup authorization. That flag is an operator assertion after physical recovery, not proof supplied by hardware. Do not configure unattended automatic fault clearing or restart as a substitute for inspection. A process restart loses the in-memory release obligation, so the physical check also applies after an unexpected termination.

The service executes a discrete pair synchronously on its control thread. Other socket requests may be delayed during the pair, including CEC/Web requests, but a CD operation is refused while volume control is active. The nominal discrete delay is 100 ms plus two frame transmissions and waveform checks; Linux scheduling and BUSY waits have no hard maximum. Lease expiry cleanup can likewise be delayed by an in-progress synchronous transmission. The release attempt still occurs after a slow PRESS verification. This phase has no receiver-side completion acknowledgment and no automatic track retry.

## Offline concurrency validation

`make test` builds a test executable using the production socket loop and a fake GPIO/monitor backend, without linking libgpiod or requesting GPIO. Its Unix-socket tests keep a CEC connection open while short-lived CD PRESS, HEARTBEAT, RELEASE and STATUS requests interleave. They check stale tokens, lease expiry, detached-hold shutdown, unrelated-client disconnects and volume-owner STOP cleanup. These tests establish software routing and safety obligations; they do not validate physical waveform timing or receiver execution.

The Gateway accepts the optional CD STATUS fields and keeps authenticated active controls ineligible while `cd_state=ACTIVE|HOLDING|FAULT_LATCHED` or a CD RELEASE remains owed. STATUS remains read-only and available. Simultaneous CD holding and volume/MUTE transmission is deliberately unvalidated and rejected because volume/MUTE enters `0x17`, while CD KEY UP is validated in ROOT.

## Physical API validation: 2026-10-12

The operator reports successful execution of the **Docker/QEMU cross-built
ARMv6 service** on Raspberry Pi B+ Rev 1.2 with the KR-V6070 and connected CD
changer. The [dated hardware report](../research/notes/xs8-cd-hold-api-validation-2026-10-12.md)
records the evidence and supplied final STATUS fields.

- `CD_TRACK_NEXT`: physically advanced one track.
- `CD_TRACK_PREVIOUS`: physically restarted the current track in the tested state.
- Explicit `CD_PRESS` / `CD_RELEASE`: successful KEY DOWN / KEY UP reported.
- Lease timeout: automatic logical `0x72` RELEASE reported.
- Fresh PRESS after expiry: accepted with a new token.
- Six HEARTBEAT renewals: successful over approximately 3 seconds.
- SEEK_FORWARD: physically stopped after explicit RELEASE.

Final STATUS reported `state=IDLE`, `monitor=HEALTHY`, `cd_state=IDLE`,
`cd_release_owed=0`, `cd_lease_expired=0`, `cd_last_result=CLIENT_RELEASE`,
`last_byte=0x72`, `last_waveform=MATCH` and `receiver_integrity=HEALTHY`.
These are software/monitor observations. The operator's physical NEXT,
PREVIOUS and seek observations separately establish execution in those trials;
TX_OK or MATCH alone never establishes receiver execution.

Validation remains specific to this setup and the reported trials. No raw
captures, complete per-trial replies, measured discrete gaps, deployed binary
checksum or exact runtime/startup configuration were supplied. One-track
behavior is not guaranteed across scheduling delays or playback states; the
3-second renewal test does not establish indefinite physical endurance. This
report does not newly validate held SEEK_BACKWARD/held TRACK_PREVIOUS, fault
injection, shutdown cleanup, power-loss recovery, concurrent active CEC/Web
control or another receiver/changer. Existing RELEASE/fault interlocks and the
software inability to guarantee physical termination remain applicable.
CONTEXT research restrictions are unchanged; no `0x17` research entry was
validated or enabled by these ROOT trials.
