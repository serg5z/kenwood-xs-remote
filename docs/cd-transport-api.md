# CD transport socket API, phase 2

`xs8-service` remains the only XS8 GPIO owner. Commands use its **local Unix socket** and ROOT context. The [protocol evidence](xs8-protocol.md#cd-transport-press-and-release-in-root) is specific to the KR-V6070 and connected changer. The Web Remote exposes `CD_TRACK_NEXT`/`CD_TRACK_PREVIOUS` ([Phase A](web-api.md#cd-track-nextprevious-phase-a)), `CD_PLAY_PAUSE`/`CD_STOP` and held `SEEK_FORWARD`/`SEEK_BACKWARD` ([playback and seek](web-api.md#cd-playback-and-held-seek)), each behind its own default-off Gateway gate. `CD_PLAY` and the held TRACK actions remain socket-only.

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
| `CD_PLAY\n` | `0x41` | One PLAY key frame. |
| `CD_PLAY_PAUSE\n` | `0xBC` | One PLAY/PAUSE toggle key frame. **Provisional mapping** (see below). |
| `CD_STOP\n` | `0xA1` | One CD STOP key frame (formerly `0x9C`, see below). |

### Connection-owned seek: KEY_DOWN / KEY_UP

`KEY_DOWN SEEK_FORWARD|SEEK_BACKWARD` and `KEY_UP SEEK_FORWARD|SEEK_BACKWARD` start and end the same held seek as `CD_PRESS`, with the same KEY DOWN (`0x7A`/`0x6A`), the same `0x72` release path, uncertainty replies and CD fault latching. The difference is **ownership**: the hold belongs to the socket connection that sent `KEY_DOWN`, not to a token. No token is revealed.

| Request (on the owning connection unless noted) | Reply | XS8 TX |
|---|---|---|
| `KEY_DOWN A` while CD is idle | `OK KEY_DOWN_TX action=A gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED` | KEY DOWN once |
| `KEY_DOWN A` again, same action | `OK KEY_ALIVE action=A` (renews inactivity) | none |
| `KEY_DOWN` while any CD hold exists (other action, other connection, token hold) | `ERR KEY_BUSY` | none |
| `KEY_UP A` | `OK KEY_UP_TX action=A …` or `ERR KEY_UP_UNCERTAIN action=A …` | `0x72` |
| `KEY_UP` naming the other action | `ERR KEY_ACTION_MISMATCH` | none |
| `KEY_UP` from another connection, or for a token hold | `ERR KEY_NOT_OWNER` | none |
| `KEY_UP` with no hold (late, repeated, after expiry) | `OK KEY_UP_NONE last=<cd_last_result>` | none |

An uncertain KEY DOWN replies `ERR KEY_DOWN_UNCERTAIN …`; the service has already attempted `0x72` and latched a fault, as for `CD_PRESS`. Any other action name gets `ERR KEY_UNKNOWN_ACTION`.

A connection-owned hold is released by `KEY_UP`; by its **owner connection closing**, which the event loop handles immediately with reason `CLIENT_DISCONNECT`; by **1200 ms without `KEY_DOWN`** (`INACTIVITY`, which also sets `cd_lease_expired=1`); by the **10 s maximum** (`MAX_HOLD`); and by monitor failure or shutdown as before. A non-owner's `KEY_UP` or disconnect never affects it. After an `INACTIVITY` or `MAX_HOLD` release, the former owner's next `KEY_DOWN` gets `ERR KEY_EXPIRED last=<reason>` and transmits nothing until that connection sends `KEY_UP` or disconnects, so a late keepalive can never start a new press. Other connections are not blocked.

Token holds are unchanged: `CD_PRESS` keeps its 2000 ms lease, `CD_HEARTBEAT`/`CD_RELEASE` keep their replies, and disconnecting does not release them. The two kinds never release each other: `KEY_UP` answers `KEY_NOT_OWNER` for a token hold, and `CD_HEARTBEAT`/`CD_RELEASE` answer `CD_NOT_OWNER` for a connection-owned hold whatever token is given. `KEY_DOWN` is subject to the same shutdown and research-uncertainty gates as `CD_PRESS`; `KEY_UP` is answered even while research CONTEXT is open, transmitting only to end the caller's own hold.

**Why KEY_UP is never blocked by an exclusive mode.** The service loop's exclusive modes (a QUERY_STATUS collection, a research CONTEXT preparation, a research chain, a CONTEXT session) refuse most commands, and `KEY_UP` is not on their allowlists. That is safe because of two invariants:
1. **None of them can start during a CD hold.** A hold always has `cd_active=1` and an owed release. `QUERY_STATUS` answers `ERR CD_TRACK_BUSY`, preparation `UNRESOLVED_CONTROL_STATE`, `RESEARCH_SEND`/`RESEARCH_CHAIN` `reason=CD_TRACK_BUSY`, and `CONTEXT_SESSION_ENTER` `reason=CD_BUSY`.
2. **No hold can start during any of them.** Each refuses `KEY_DOWN` and `CD_PRESS`.

`KEY_UP` itself is handled before the CONTEXT, fault and monitor checks, and is not subject to the shutdown or research-uncertainty gates. A fault or monitor failure has already ended any hold through `0x72`. During shutdown, the service closes every client, which releases a connection-owned hold, before its final cleanup. A non-owner's `KEY_UP` never transmits. The socket tests check both invariants.

### Single-frame keys: PLAY, PLAY/PAUSE and STOP

These send **one** ROOT frame and no KEY UP: no `0x72`, no `0x17` entry, no hold, lease or token. They use the same preflight as the other CD commands (local client, healthy monitor, receiver control `IDLE`, no volume STOP obligation, no open context, no CD hold, pair or fault, not shutting down, no uncertain research TX). A verified frame replies `OK CD_TRANSPORT_TX action=PLAY|PLAY_PAUSE|STOP gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED`. An unverified one replies `ERR CD_TRANSPORT_UNCERTAIN` with the same fields and is **never retried**. Because no release obligation exists, it does **not** latch a CD fault, like `SELECT_SOURCE`; `STATUS` shows `cd_action` and `cd_last_result=TX_VERIFIED|TX_UNCERTAIN`.

Evidence differs by key:
- **`0x41` PLAY**: physically observed on 2026-10-12 (PLAY lit the changer indicator), in the reported CD-selected setup ([ROOT validation note](../research/notes/xs8-root-validation-2026-10-12.md)). The effect with another source selected is untested.
- **`0xA1` STOP** (current): ROOT `0x21` and `0xA1` both physically stopped CD when sent through the research REPL, and `CD_STOP` with `0xA1` has since been physically verified on the KR-V6070. Its waveform `0xA1` is the same value as the receiver's VIDEO1 source broadcast (legacy `7A`, [matrix D.2](xs8-command-matrices.md#d2-bytes-that-are-both-a-tx-command-and-a-known-rx-value)); the service's own frame is excluded from receiver state by its TX window, like every local frame.
- **`0x9C` STOP/CANCEL** (research history, no longer sent by `CD_STOP`): physically observed on 2026-10-12 (`0x41 → 0x9C` ended in STOP; `FC → 9C` cancelled a skip and stopped playback). A later report found that a VIDEO1/VIDEO2 selection after a `0x9C` STOP can provoke an unwanted changer restart, which motivated the change to `0xA1`.
- **`0xBC` PLAY/PAUSE**: a **historical** toggle mapping only. Its pause and resume RX reports conflict with older passive labels, and it has **not** been physically verified through this service. The operator has since reported that `CD_PLAY_PAUSE` was physically tested (stated 2026-10-15). No dated trial note is archived, and the Gateway still labels it `PROVISIONAL`. Record the trial before lifting that label.

The discrete commands complete both frame attempts before replying. After a verified PRESS, they request a **100 ms minimum gap from the recorded end of KEY DOWN TX to the beginning of KEY UP TX**. Waveform verification, BUSY-free checks, and scheduling can make the actual gap longer. If PRESS transmission or waveform verification is uncertain, RELEASE is attempted immediately. The explicit `CD_PRESS` commands send KEY DOWN **once** and hold while their lease is renewed. They do not use the 100 ms gap or repeat KEY DOWN. No `0x17` context entry or CD STOP is substituted.

On a fully matched pair, a reply has this form:

```text
OK CD_TRACK_TX action=NEXT press_gpio_tx=TX_OK press_waveform=MATCH release_gpio_tx=TX_OK release_waveform=MATCH cd_result=TX_VERIFIED receiver_execution=UNVERIFIED
```

`PREVIOUS` uses `action=PREVIOUS`. `TX_VERIFIED` means both GPIO transmissions completed and their bus waveforms matched; it is **not** an acknowledgment from the receiver or a guaranteed track movement. An uncertain pair returns `ERR CD_TRACK_UNCERTAIN` with the same evidence fields and `cd_result=PRESS_UNCERTAIN` or `RELEASE_UNCERTAIN`. A failed PRESS still triggers one RELEASE attempt. A failed or missing RELEASE match latches `cd_state=FAULT_LATCHED` and retains `cd_release_owed=1` until a confirmed transmission or process restart. A PRESS failure also latches the CD fault even when RELEASE matches, because the operation's outcome remains uncertain.

An explicit verified press replies `OK CD_PRESS_TX action=TRACK_NEXT hold_token=1 gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED`, where the decimal token is an example. The client sends `CD_HEARTBEAT 1` about every **500 ms** and receives `OK CD_LEASE_RENEWED`; this transmits no XS8 frame. It sends `CD_RELEASE 1` for immediate KEY UP and receives `OK CD_RELEASE_TX reason=CLIENT_RELEASE gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED` when the waveform matches. An explicit hold has an **absolute maximum of 10 s** (`Xs8ServiceCore::kCdMaxHold`), measured on the monotonic clock from the successful `CD_PRESS` request, the same start point the lease uses. Heartbeats renew only the 2000 ms lease, never this limit. When it is reached, the service sends `0x72` through the same RELEASE path with reason `MAX_HOLD`. The limit applies to every explicit `CD_PRESS` action, not only seek. Uncertain results reply `ERR CD_PRESS_UNCERTAIN` or `ERR CD_RELEASE_UNCERTAIN` with available TX/waveform fields. A duplicate press while held returns `ERR CD_TRACK_BUSY`; a release with no active hold returns `ERR CD_NOT_ACTIVE` and sends no frame. A missing/malformed token returns `ERR CD_TOKEN_REQUIRED`; a stale or wrong token returns `ERR CD_NOT_OWNER`. An uncertain PRESS triggers immediate RELEASE and latches a fault. A failed RELEASE retains its obligation and latches a fault. No automatic retry of KEY DOWN occurs.

`STATUS` includes `cd_state=IDLE|ACTIVE|HOLDING|FAULT_LATCHED`, `cd_action`, `cd_release_owed=0|1`, `cd_lease_expired=0|1`, and `cd_last_result`. It does not expose the hold token. A CD fault also reports the existing overall `state=FAULT_LATCHED` so Gateway readiness cannot mistake it for ordinary IDLE. Other status fields remain. New CD requests require a local client, healthy monitor, receiver control `IDLE`, no volume STOP obligation, and no open `0x17` context. Shutdown and an earlier uncertain research TX also block new CD presses. A CD hold blocks conflicting active commands, including CEC volume START, MUTE, source selection, queries, and research TX; they receive prompt error replies. STATUS and token-matched HEARTBEAT/RELEASE remain available from other connections. A research chain already in progress rejects CD commands under its existing session-busy rule. The single owner thread sends no unrelated frame between KEY DOWN and KEY UP.

The service accepts up to **eight simultaneous local socket clients** and still runs every GPIO command on one control thread. A CD hold belongs to its service-owned token, not to the connection that requested PRESS. That connection may close; a short-lived request from another client can renew or release the hold with the token. Each hold receives an independently generated, nonzero 64-bit token before KEY DOWN; token-generation failure rejects PRESS without transmission. Keep this token private: possession permits renewal or release over any authorized local socket connection. STATUS and logs do not expose it. Local socket permissions and future Gateway authorization remain necessary. The persistent CEC client and short-lived Gateway requests can receive STATUS or prompt busy responses throughout a CD hold. A future Web hold API (Phase B) must bind the token to an authenticated browser session and send bounded heartbeats; no Web hold endpoint is implemented. The Web Remote uses only the discrete commands for short taps and never receives a hold token.

## Failure, shutdown, and operator recovery

The release obligation is set **before** KEY DOWN may reach the bus. The service attempts `0x72` after a failed or uncertain PRESS and after an interrupted discrete wait. For explicit holds it also attempts RELEASE on token-matched client request, monitor failure, **2000 ms without a valid heartbeat**, and orderly shutdown. Socket disconnect alone does not release a CD hold; the lease timeout covers a lost short-lived client. The **10 s maximum** bounds a hold whose heartbeats never stop; it is checked by the same control loop and before every command, so a heartbeat that arrives after the maximum receives `ERR CD_NOT_ACTIVE` and cannot restart or extend the hold. If the lease lapsed before the maximum, the release reports `LEASE_EXPIRED` even when the control loop notices it later. `cd_last_result=MAX_HOLD` (with `cd_lease_expired=0`) describes a matched maximum-hold release until a new CD operation is accepted; an unconfirmed one reports `RELEASE_UNCERTAIN` and latches a fault like any other RELEASE failure, and the journal line `XS8 CD_RELEASE reason=MAX_HOLD` keeps the reason. The heartbeat and lease clocks are monotonic. The control loop checks expiry approximately every 20 ms when otherwise idle. After a matched expiry RELEASE, a fresh CD PRESS or discrete tap is permitted subject to all existing preflight checks. `cd_lease_expired=1` and `cd_last_result=LEASE_EXPIRED` describe the completed lease until a new CD operation is accepted; they are not a fault or a restart requirement. An accepted operation clears the expiration flag and a new hold receives a fresh token. HEARTBEAT/RELEASE with an expired token returns `CD_NOT_ACTIVE` while idle or `CD_NOT_OWNER` during a new hold, without transmission or renewal. An uncertain RELEASE still latches a CD fault and retains its obligation. Other CEC and Web operations can resume after a matched release, subject to their existing safeguards. Monitor failure during a hold also latches a fault. If the first RELEASE is unconfirmed, a graceful shutdown makes at most one additional RELEASE attempt. Failure leaves a visible fault and a prominent log warning. The service does **not** automatically send a CD STOP, repeat the track command, or interpret `TX_OK`/`MATCH` as physical execution.

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
