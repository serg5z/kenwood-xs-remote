# XS8 research REPL v1.1

[`xs8_repl.py`](../xs8_repl.py) is a local, interactive client of the existing XS8 Service Unix socket. It never opens GPIO. The service remains the single GPIO owner and retains its CEC/Web command serialization, volume STOP obligations, watchdogs, waveform verification, and fault policy. Research TX is **off by default** and requires the separate `xs8-service --enable-research-tx` startup flag. That flag does not leave BOOT_INHIBIT: the existing `--allow-unrecovered-start` operator authorization and all IDLE/monitor/STOP/context checks still apply. Do not add the research flag to an unattended production service without a reviewed, attended research session.

From the repository root, run `python3 xs8_repl.py [--socket /tmp/kenwood-xs8.sock]`. Starting the REPL sends no XS8 command. Type `help` for `status`, `query`, `send 0xNN [milliseconds]`, `observe N`, `set observe N`, `history`, and `quit`. The default post-TX observation window is 1500 ms; the server accepts windows of 1–5000 ms. `query` invokes the existing `QUERY_STATUS` request and does not depend on research TX authorization. `observe` is passive and can operate with research TX disabled if the monitor is healthy.

The attended **preparation-only** `context` commands below are available. They never enter receiver context or transmit the proposed experiment. General `context enter/send` remains unimplemented, and raw logical `0x17` remains forbidden in both single-byte and chained research TX.

Example transcript (illustrative; **not** a recorded hardware result):

```text
xs8> status
OK state=IDLE stop_owed=0 context_may_be_open=0 monitor=HEALTHY ...
xs8> set observe 1500
Default observation window: 1500 ms
xs8> observe 1000
OK RESEARCH_OBSERVE id=1 start_ns=... deadline_ns=...
OK RESEARCH_DONE id=1 count=0 ...
xs8> send 0x9C 1500
OK RESEARCH_TX gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED id=2 tx_start_ns=... tx_end_ns=... deadline_ns=... context=ROOT_ASSUMED
OK RESEARCH_RECORD id=2 index=0 kind=FRAME start_ns=... end_ns=... waveform_lsb=0x9C legacy_msb_short1=0xC6 valid=1 bits=8 lost_events=0 reason=NONE relation=TX_WINDOW origin=UNATTRIBUTED receiver_execution=UNVERIFIED
OK RESEARCH_DONE id=2 count=1 ...
```

`RESEARCH_SEND 0xNN N` takes a monitor checkpoint **before** its one logical TX frame. After the existing TX waveform check completes, the service returns an ID and drops the socket connection when the client closes it. The REPL polls `RESEARCH_READ ID INDEX` through short new connections, freeing socket capacity during the 1–5000 ms observation window. The service accepts up to eight local clients, so STATUS and other requests can interleave with those polls; all TX remains serialized on its existing control thread. If socket capacity is temporarily full, the REPL retries only the read request for a bounded time; it never resends the research TX. Research TX is rejected during a leased CD hold. Only one research observation session is active at a time. The most recent completed session remains readable until replaced by another; a new session cannot begin while one is active.

Each record preserves kernel monotonic start and end timestamps, `waveform_lsb`, `legacy_msb_short1`, validity, bit count, decoder reason, and lost-edge count for a gap. `relation=TX_WINDOW` means only that its start fell inside the local TX timing window. `origin=UNATTRIBUTED` remains explicit for every bus frame; later traffic is not called an acknowledgment. `gpio_tx=TX_OK` reports GPIO completion, `waveform=MATCH` reports a matching observed bus frame, and `receiver_execution=UNVERIFIED` states the remaining limit. A completed frame extending beyond the observation deadline is reported incomplete. Monitor failure, history wrap, and the 256-record session cap are reported by `monitor_failed=1` or `overflow=1`; records are never silently presented as a complete capture after those faults.

The service rejects research TX in BOOT_INHIBIT, FAULT_LATCHED, active volume, an unhealthy monitor, or an unresolved STOP/context state. It also rejects `0x17` in v1, because that known byte enters a different command context and would undermine the promised ROOT workflow. Other unknown ROOT bytes can still change state or latch a device; `context=ROOT_ASSUMED` is an explicit assumption, not feedback from the receiver. A failed or mismatched research TX blocks further research TX until the service is investigated and restarted. There is no automatic retransmission, sweep, or cleanup command for an arbitrary research byte. The service logs research SEND, OBSERVE, and CANCEL requests to stderr. Ctrl+C while polling sends `RESEARCH_CANCEL ID` to stop collecting that observation; if pressed during a SEND request, the service may already have transmitted the frame. Cancellation does not cancel an XS8 operation in progress.

The proposed first physical validation is a single ROOT `0x9C` during ordinary CD playback, with the receiver and changer attended. Before any TX, confirm a reachable physical CD STOP and independent changer power removal path; the archive has **not** established a guaranteed recovery from a latched TRACK/SEEK operation. Record the changer's physical stop separately from `TX_OK`, waveform match, and any RX `D6`. Do not test `0xFC → 0x9C` until physical recovery has been demonstrated. See the [CD transport audit](../research/COMMAND_REGISTRY.md#L50-L69) for historical command evidence. No physical test is run by this implementation.

## Local protocol extension

All replies remain one line. `RESEARCH_OBSERVE N` starts a passive session. `RESEARCH_SEND 0xNN N` starts one TX and post-TX observation. The response includes `id`, timing, GPIO TX result, and waveform result; an uncertain TX returns `ERR RESEARCH_TX` **with an ID** so its captured bus activity can still be read. `RESEARCH_READ ID INDEX` returns one `RESEARCH_RECORD`, `RESEARCH_WAIT` while collection continues, or `RESEARCH_DONE` with record count and integrity flags. `RESEARCH_CANCEL ID` ends observation without any TX. Requests use separate connections and do not replay a command. Malformed requests return `ERR RESEARCH reason=INVALID_REQUEST`. The existing `STATUS`, `QUERY_STATUS`, volume, source, and MUTE wire formats are unchanged.

## Chained TX (v1.1)

Chained transmission additionally requires `xs8-service --enable-research-chain` alongside the existing default-off `--enable-research-tx` flag. Neither flag belongs in normal unattended startup. The REPL accepts 1–4 logical bytes; `send 0x9C` and `send 0x9C 1500` retain their v1 behavior. For two or more bytes, use `send 0xFC 0xEC --gap-ms 200 --observe-ms 1500`. This is syntax only, **not** a recommended physical test. The gap defaults to 200 ms and accepts 50–1000 ms; the observation window follows the last attempted TX and accepts 1–5000 ms. The gap is a minimum from the recorded end of one GPIO TX to the start of the next. Verification, bus-free checks, and service scheduling may extend it.

The REPL sends one `RESEARCH_CHAIN COUNT GAP_MS WINDOW_MS 0xNN...` request. The service takes one monitor checkpoint before the first TX, immediately returns `OK RESEARCH_CHAIN id=...`, and performs the sequence on its existing GPIO owner thread. The request socket can close while the session continues. `RESEARCH_TX_READ ID INDEX` reports each attempted byte as `RESEARCH_TX_RECORD` with requested byte, actual TX start/end monotonic timestamps, `gpio_tx`, waveform evidence, and `receiver_execution=UNVERIFIED`; it ends with `RESEARCH_TX_DONE count=... planned=... aborted=... reason=...`. `RESEARCH_READ ID INDEX` waits until the sequence stops, then reports captured frames, decoder errors, lost-edge gaps, and integrity flags. This prevents a later-known capture deadline from invalidating a record already returned to the client. `relation=TX_WINDOW` means a frame began in any locally recorded TX window; `origin=UNATTRIBUTED` remains explicit.

The service checks IDLE, monitor integrity, STOP obligation, and context before every byte. It aborts the remaining sequence after any GPIO failure, uncertain waveform, failed safety check, monitor failure, shutdown, or cancellation. Other active commands are rejected during a chain; `STATUS` and volume RELEASE remain available, and service watchdog/STOP processing continues between frames. An individual `xs8_gpio_send()` and waveform verification is synchronous and may briefly delay socket processing. There are no automatic retries, scans, or speculative cleanup bytes. A matching waveform does **not** establish receiver execution or prove that an arbitrary following command is safe. Multi-byte sequences can produce latched hardware behavior and require attended research with an independent physical recovery plan. In particular, do not test `0xFC → 0x9C` until physical recovery from the first command is established.

## Offline checks and Pi validation

Run `make test` and `python3 -m unittest tests/test_xs8_repl.py` with no receiver connected. On the Raspberry Pi, build with `make .build/xs8-service`; validate the CLI first against a fake Unix-socket service. Before enabling research TX on the real XS8 Service, review its effective startup command, socket ownership, monitor health, receiver state, and physical recovery plan. Keep the research flag absent from normal startup, and remove it again after the attended session. The current implementation is not deployed by this document.

## Attended MUTE experiment preparation: NO TX

The first named profile is **MUTE**, whose characterized transaction is exactly logical `0x17 → 0xD8 → 0x07`, using the working volume controller's existing entry/exit path. No arbitrary byte, source-selection candidate, volume START, or seek command is admitted. This phase implements preparation and passive diagnostics only: `context run` always refuses TX with `RECOVERY_EVIDENCE_MISSING`. Ordinary production MUTE retains its existing behavior; preparation does not execute it or create a new route around the raw `0x17` prohibition.

| REPL command | Implemented behavior |
|---|---|
| `context prepare MUTE` | Ask for the exact phrase `PREPARE MUTE NO TX`; reserve the service under a private token and immediately record one passive window using the configured observation duration. |
| `context status` | Report the preparation state and independent context, panel and normal-control evidence fields. It sends no XS8 frame and does not renew inactivity. |
| `context observe [N]` | Record another passive window under the token; no frame decoding semantics are invented. |
| `context run` | Refuse transmission, even with production operator/research startup flags enabled. |
| `context cancel` / `context exit` | Cancel passive preparation. No `0x07` is sent because this preparation never entered hardware context. |

The service owns preparation across short-lived sockets. It requires IDLE, no `stop_owed`, no possibly open context, no CD fault/RELEASE obligation, no active research/query, and healthy monitor/receiver-observation integrity. A preparation token permits only its heartbeat, passive capture, blocked RUN and cancellation; it is absent from service logs, STATUS and console history. Explicit confirmation is an attended-use assertion, not authentication; existing Unix-socket permissions remain required.

While reserved, normal STATUS reports `state=RESEARCH_PREPARATION`, so the existing Gateway readiness check disables active controls. STATUS and passive record reads remain responsive. Conflicting CEC volume PRESS, MUTE, CD, source, QUERY_STATUS and unrelated research mutations receive `EXCLUSIVE_PREPARATION`. Volume RELEASE remains available to preserve existing STOP handling. A 2-second inactivity lease and **10-second absolute lifetime** bound the reservation; the REPL renews inactivity about every 500 ms while reading passive observations. Renewals cannot extend the absolute deadline. No background heartbeat keeps an abandoned console reserved indefinitely.

Cancellation, Ctrl+C, quit/EOF, missing heartbeat, absolute expiry, clock/monitor failure, receiver-integrity loss, capture overflow and service shutdown release preparation without any cleanup TX. Passive frames—including malformed frames—retain both decoder byte conventions, timestamps and gap/error diagnostics through the existing research record format. `context_entered=0` and `tx_attempted=0` describe this experiment's lack of transmission; they do not measure the receiver's pre-existing context or prove an unlocked front panel. All recovery-check fields remain `NOT_OBSERVED`; releasing a software reservation is never reported as successful physical recovery.

Illustrative preparation only:

```text
xs8> context prepare MUTE
Type PREPARE MUTE NO TX to confirm attended preparation: PREPARE MUTE NO TX
OK RESEARCH_CONTEXT_PREPARED token=<private> profile=MUTE mode=PREPARATION_ONLY ... context_entered=0 recovery_ready=0
OK RESEARCH_OBSERVE id=... mode=CONTEXT_PREPARATION_PASSIVE tx_attempted=0
OK RESEARCH_DONE ...
xs8> context run
ERR RESEARCH_CONTEXT reason=RECOVERY_EVIDENCE_MISSING profile=MUTE ... context_entered=0
xs8> context cancel
OK RESEARCH_CONTEXT_CANCELLED context_entered=0 tx_attempted=0
```

### Smallest missing observation and exact attended-test prerequisites

Normal MUTE entry/exit is already characterized, but the archive lacks a single recorded trial tying that transaction to all three independent postconditions below. Status response alone cannot supply them: the documented front-panel lock still allowed `0x13` replies. Before reviewing any transmitting research profile, archive **one attended use of the existing production MUTE transaction** with raw/decoded bus evidence and these checks:

| Independent check | Required evidence | Insufficient evidence |
|---|---|---|
| Logical context returned to ROOT | After the transaction, one explicit existing `QUERY_STATUS` returns the expected two observations for the physically known source/MONITOR state. | `0x07` TX_OK/MATCH or silence after `0x17`. |
| Front-panel operation / lock | An attended, non-motor front-panel source selection visibly works; record the display/state and any passive broadcast. | A ROOT status reply, which also worked while locked. |
| Normal receiver control | An existing, validated ROOT source operation visibly works after the front-panel check—for example CD `0x84` from a different known source, without a tuner-preset side effect. | A command name, GPIO TX_OK or waveform MATCH without physical observation. |

Prerequisites for that separate hardware observation:

1. The receiver and changer are attended, volume is at a safe level, no volume or CD hold is active, and front-panel controls and an independent physical recovery procedure are reachable. Record source, MONITOR, MUTE and front-panel responsiveness before the trial. The historical standby/wake lock recovery is state-bound evidence, not an automatic service recovery action.
2. Existing service operator authorization is established independently; STATUS is IDLE, monitor healthy, no STOP/context/CD RELEASE obligation and receiver-observation integrity healthy. Preparation cannot bypass BOOT_INHIBIT. Arrange that CEC/Web operators issue no concurrent commands during the one trial.
3. Cancel/expire the NO-TX preparation reservation first. Its RUN remains blocked. Use only the already implemented production MUTE path for this separate observation, exactly once; do not inject raw `0x17`, add unknown bytes, toggle MUTE twice automatically or retry an ambiguous result.
4. Start an existing passive observation window before the MUTE transaction and archive intended TX, observed frames, both byte conventions, timestamps, gaps/timing diagnostics and operator observations. Extend evidence collection with additional passive windows as needed for the physical checks; do not substitute a scan.
5. Record the three postconditions separately. If any fails or traffic/timing is uncertain, stop testing, retain the evidence and recover physically using the attended procedure. An ambiguous trial does not authorize the new transmitting profile.
6. Review the archived result and failure handling before implementing/enabling one default-off transmitting MUTE profile. VIDEO 1 / VIDEO 2 remain outside the allowlist; their TX bytes and recovery behavior require separate evidence.

The implemented socket requests are `RESEARCH_CONTEXT_PREPARE MUTE CONFIRM_NO_TX`, `RESEARCH_CONTEXT_STATUS`, `RESEARCH_CONTEXT_OBSERVE TOKEN N`, `RESEARCH_CONTEXT_HEARTBEAT TOKEN`, `RESEARCH_CONTEXT_RUN TOKEN` (always blocked), and `RESEARCH_CONTEXT_CANCEL TOKEN`. No byte-bearing context SEND is supported. Fake-backend tests check zero experiment TX, exclusivity with interleaved STATUS/CEC clients, existing STOP/CD RELEASE obligations, expiry, stale tokens, diagnostic failures and shutdown. They do not establish the missing physical postconditions or claim tested recovery after a new experiment TX failure.

## Interactive CONTEXT research session (attended, default off)

This mode lets an operator explore commands inside the `0x17` context one byte at a time. It needs no new transaction code or rebuild per candidate. It is research tooling for an **attended** receiver only. The successful MUTE recovery validates that one transaction; it does **not** show that `0x07` undoes an arbitrary byte. Raw `0x17` stays forbidden on `RESEARCH_SEND` and `RESEARCH_CHAIN` (`CONTEXT_ENTRY_FORBIDDEN`). Entry uses only the dedicated session API below.

**Authorization.** All four are required:
- the service runs with both default-off flags `--enable-research-tx --enable-research-context` (the startup line shows `research_context=ENABLED`);
- the existing operator authorization, so the service is IDLE rather than BOOT_INHIBIT;
- the request carries `CONFIRM_ATTENDED`;
- the REPL operator types `ENTER CONTEXT ATTENDED`.

Keep the flags out of normal startup and remove them after the session.

**Entry preconditions.** IDLE; no volume STOP or context obligation; no CD pair, hold, fault or owed RELEASE; monitor and receiver integrity healthy; no research capture, chain, query or MUTE preparation active.

**REPL commands.**

| REPL | Socket request (owner connection) | Behavior |
|---|---|---|
| `context enter` | `CONTEXT_SESSION_ENTER CONFIRM_ATTENDED [ms]` | Sets `context_may_be_open=1`, then sends `0x17` through the normal BUSY-checked, waveform-verified transmitter. Returns a private token, lease and lifetime. |
| `send 0xNN [--observe-ms N]` (while a session is open) | `CONTEXT_SESSION_SEND TOKEN 0xNN [ms]` | Sends exactly one operator-chosen byte on the owner connection. No allowlist, chain, sweep or retry. |
| (automatic, every 1 s) | `CONTEXT_SESSION_HEARTBEAT TOKEN` | Renews the 3 s inactivity lease; cannot extend the 300 s absolute lifetime. |
| `context exit` | `CONTEXT_SESSION_EXIT TOKEN [ms]` | Sends `0x07` (up to 3 attempts, as the volume and MUTE cleanup do). Clears `context_may_be_open` only after a verified frame. |
| `context status` | `CONTEXT_SESSION_STATUS` | Read-only: open, sends, remaining lease and lifetime, last result, state. Never shows the token. |
| `context verify` | `QUERY_STATUS` | After exit only: the ROOT query, plus the manual checklist below. |

`[ms]` is the passive observation window after the command (1–5000, default 1500).

**One `send` command, two meanings.** Outside a session, `send` keeps its ROOT research behavior (`RESEARCH_SEND`, or `RESEARCH_CHAIN` for 2–4 bytes). While a session is open, `send` goes only to `CONTEXT_SESSION_SEND` with the session token. It accepts exactly one byte, plus an optional `--observe-ms N`. Chains, `--gap-ms` and the positional window form are refused locally without transmission. Denied bytes are also refused locally with a reason. The service enforces the same denylist, and its refusal reasons are explained the same way. `context send` no longer exists.

**Never a silent fallback to ROOT.** The console latches *CONTEXT state uncertain* whenever a session ends without a clean `OK CONTEXT_SESSION_EXITED`. That covers an uncertain or refused send, a lost lease or heartbeat, a lost owner connection, an uncertain entry, and an EXIT that is unverified or unavailable. While latched, `send`, `context enter` and `context verify` are refused locally and nothing is transmitted. Only `context status` reporting `open=0 context_may_be_open=0` from the service clears the latch. That status doesn't show the receiver is back to normal, so complete `context verify` and the physical checks before trusting ROOT behavior.

Example session:

```text
xs8> context enter
xs8> send 0x78
xs8> send 0x88
xs8> context status
xs8> context exit
xs8> context verify
```

**Not sendable as candidates.** This is a short denylist, not an allowlist: `0x17` (nested entry), `0x07` (use EXIT) and `0xE9`/`0xF9`. The last two start the volume motor, which needs `0x09` under the production hold watchdog.

**Known hazards are not blocked.** Other bytes with known effects can still be sent, and the operator is responsible for them:
- in context, `0x39` powers the receiver off, and `0x15` starts a tuner preset scan;
- the ROOT LOCK* family;
- CD continuous `0xEC`/`0xFC`/`0x6A`/`0x7A`, which need ROOT `0x72` to stop.

**Ownership and exclusivity.**
- The token works **only** on the connection that entered. Any other connection gets `NOT_OWNER`, with no TX and no renewal.
- While the session is open, STATUS reports `state=RESEARCH_CONTEXT context_may_be_open=1`. That disables Gateway and Web controls.
- Every other transmitting request gets `ERR RESEARCH_CONTEXT_ACTIVE`, or a research/QUERY state error: MUTE, volume PRESS, source, CD, `QUERY_STATUS`, research send/chain, MUTE preparation.
- `VOLUME_RELEASE` still answers as it does in IDLE, without TX.
- STATUS and capture reads stay available.

**Service-owned exit.** The service sends `0x07` itself, on its GPIO owner thread, when any of these happens:
- the owner connection closes (console exit, crash, kill);
- the 3 s lease lapses (no heartbeat, e.g. a suspended console);
- the 300 s lifetime ends;
- the monitor fails (this also latches FAULT);
- an orderly shutdown.

Checks run on the ~20 ms service loop, delayed by at most one in-progress frame (~110–130 ms).

**Faults.**
- An uncertain entry frame, an uncertain candidate (`TX_FAIL`, or anything but `MATCH`) or an unverified `0x07` latches `FAULT_LATCHED`. An uncertain entry or candidate still gets one service `0x07` exit attempt, and the candidate is never retried.
- An unverified `0x07` also keeps `context_may_be_open=1`.
- Faults are never cleared over the socket. Recover physically (standby → wake, front-panel check), then restart under the existing operator procedure.
- A crash, SIGKILL, power loss, GPIO failure or permanently busy bus can still prevent `0x07`. A restart begins in BOOT_INHIBIT.

**Records.** Every ENTER, SEND and EXIT reply lists each transmitted frame, including every `0x07` attempt, as `frames=0xNN:gpio_tx:waveform:start_ns:end_ns`. It also carries a `capture_id` for the passive capture (both byte conventions, timestamps, `relation=TX_WINDOW` for local frames, gaps and integrity). The REPL reads that capture automatically.

The service journal logs each send and exit with reason, root attempts, evidence and final state. It never logs the token.

`gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED` is electrical evidence only. Record what the receiver physically did separately.

**ROOT verification after every session.** `EXITED ... root_verified=NOT_OBSERVED` means only that `0x07` matched. Run `context verify` and check all three, independently:
1. **ROOT query:** `QUERY_STATUS` returns two frames that match the physically observed source/MONITOR.
2. **Front panel:** a non-motor source button visibly works, so the panel isn't locked.
3. **ROOT source command:** production `SELECT_SOURCE` from a different known input selects it, e.g. CD rather than tuner preset CH8.

If any check fails, stop, keep the logs and recover physically.

**Tests.** `make test` covers this offline with a fake transmitter and the fake socket backend:
- the gates, including both flags;
- token and connection ownership;
- denied bytes;
- lease expiry and maximum lifetime;
- owner and non-owner disconnects;
- refusal of every other transmission;
- BUSY or GPIO failure on entry, an uncertain candidate, `0x07` failure after 3 attempts, monitor failure and orderly shutdown;
- per-command captures and frame records;
- token masking in REPL output and history;
- context-aware `send`: one byte only, local refusal of chains, gaps and denied bytes, and no ROOT fallback while CONTEXT is open or uncertain.

These tests check software obligations, not receiver behavior.

**Hardware validation (2026-10-12).** Attended runs exercised enter, a single `send`, explicit exit with one verified `0x07` and `context_may_be_open=0`, and `context verify`, with no fault. The runs physically established `0x78` → VIDEO1, `0x88` → VIDEO2 and `0xD8` → MUTE toggle in context ([trial record](../research/notes/xs8-context-video-mute-2026-10-12.md)). Lease expiry, disconnect, shutdown and failure exits were not exercised on hardware. One open anomaly: after an initial empty session (enter, then exit), the REPL printed `NOT_ACTIVE` while the service journal showed `reason=CLIENT_EXIT` with successful cleanup. The cause is **not established**. A heartbeat racing the exit is one unconfirmed explanation, from reading the code.

## Original context-session design notes

The rest of this section is the design analysis that preceded the implemented mode. Its recovery and evidence cautions still apply. Its proposed API names differ from the implemented `CONTEXT_SESSION_*` requests, and the implemented mode intentionally has no candidate allowlist. `CONTEXT_ENTRY_FORBIDDEN` remains enforced for `RESEARCH_SEND` and `RESEARCH_CHAIN`. [Current service](../xs8_service.cpp)

### Verified lifecycle and missing evidence

- **Reported hardware behavior:** from the tested ROOT baseline, logical `0x17` changed `0x84` from CD selection to tuner CH8 and made `0x13` silent. Logical `0x07` restored ROOT interpretation and status responses. This supplies normal entry/exit evidence for the tested states. It supplies no positive entry acknowledgment, additional handshake, or universal mode query. [Controlled probes](../research/notes/kenwood-xs-reverse-engineering-reviewed.md#L26-L53), [exit/status observation](../research/notes/kenwood-xs-reverse-engineering-v18.md#L553-L562)
- **Working software mechanism:** `VolumeControl` marks context possibly open before sending `0x17` through the normal BUSY-checked, waveform-verified transmitter. It sends its known volume or MUTE command, then closes context with `0x07`; a volume START first creates an obligation to send `0x09`. This is more than a raw `send 0x17`, but it still does not prove receiver execution. [Entry](../volume_control.cpp#L160-L195), [MUTE](../volume_control.cpp#L239-L261), [cleanup](../volume_control.cpp#L116-L157)
- **Recovery gap:** `0x07` did not release the recorded front-panel lock. `0x15` released that lock in ROOT but started preset scan after `0x17`. `0x09` is the known entered-context volume STOP, and ROOT `0x72` is validated only for four CD transport operations. No universal combination is established for an unknown entered-context byte. The implemented startup `0x17 → 0x09 → 0x07` path is explicitly experimental, not archived proof of recovery in arbitrary states. [Lock distinction](../research/notes/kenwood-xs-reverse-engineering-reviewed.md#L195-L206), [context-sensitive `0x15`](../research/notes/kenwood-xs-reverse-engineering-v18.md#L539-L562), [experimental recovery](../volume_control.cpp#L76-L113)
- **VIDEO source gap:** VIDEO1/VIDEO2 have physically validated passive/status source IDs, but those RX labels do not establish their logical TX commands or a safe exit profile after an unknown candidate. Earlier TX names and numerical patterns cannot fill that gap. [Source-selection evidence](source-selection.md), [RX validation](../research/notes/xs8-readback-2026-10-11.md)

These distinctions block enabling a general `context send 0xNN`. A narrower implementation could proceed only with a reviewed per-command allowlist whose normal termination and recovery behavior have been demonstrated. A token, timer, or fake transmitter can enforce software cleanup attempts; none supplies the missing hardware recovery evidence.

### Proposed local API and ownership

| Future REPL command | Proposed socket request | Intended behavior |
|---|---|---|
| `context enter` | `RESEARCH_CONTEXT_ENTER WINDOW_MS` | Establish a checkpoint, perform the reviewed entry transaction, and return an independent opaque session token plus electrical evidence. |
| `context send 0xNN` | `RESEARCH_CONTEXT_SEND TOKEN 0xNN WINDOW_MS` | Send exactly one allowlisted logical byte within that session and observe its bus traffic. No chaining, scanning or automatic retry. |
| `context exit` | `RESEARCH_CONTEXT_EXIT TOKEN` | Execute only the validated recovery profile and retain unresolved obligations on uncertainty. |
| `context status` | `RESEARCH_CONTEXT_STATUS` | Read session state, deadlines and recovery obligations without TX or exposing the token. |
| Internal REPL heartbeat | `RESEARCH_CONTEXT_HEARTBEAT TOKEN` | Detect disappearance while using short-lived sockets; never extend the absolute session lifetime. |

The XS8 Service would own one context session on its existing control thread. Socket close alone would not erase the session. Possession of the secret token would authorize context SEND, heartbeat and EXIT over separate local connections; old tokens would never revive a completed session. Normal STATUS would remain responsive. Entry would require the existing operator authorization, a separate default-off context authorization, IDLE, no volume STOP obligation, no possibly open context, no CD release obligation or CD hold, no active research chain/query, a healthy monitor, and healthy receiver-observation integrity. Cached source/MONITOR observations would not count as proof of ROOT.

During entry, the session would mark its own `context_may_be_open` obligation **before** the entry frame. It would use the existing service transmitter and the same conservative ordering as the working controller, without invoking volume START or toggling MUTE to force entry. Ordinary STATUS would aggregate the volume and research context obligations; it would never clear the existing volume `stop_owed` flag. CD, volume, MUTE, source selection, ROOT queries and unrelated research TX would receive prompt busy responses until cleanup resolved the session. Reserved context/control bytes, known motor commands, power commands and commands without a reviewed termination profile would be rejected by context SEND.

### Proposed lifetime and recovery

Proposed initial bounds are a **10-second absolute lifetime** from before entry TX and **2 seconds of owner inactivity**, with an internal heartbeat about every 500 ms. These are design values, not deployed settings. Observation polling may renew inactivity but cannot extend the absolute lifetime. A short-lived socket, REPL failure or ambiguous SEND reply would not remove the service's cleanup obligation.

The proposed owner-thread states are `CLOSED → ENTERING_UNCERTAIN → ACTIVE_ASSUMED → EXIT_PENDING → ROOT_OBSERVATION_PENDING → CLOSED`, with `FAULT_LATCHED` on unresolved cleanup. The state names describe software obligations; `ACTIVE_ASSUMED` must not claim receiver acknowledgment. Exit, expiry, lost heartbeat, monitor/capture failure, Ctrl+C/quit cleanup, failed TX and orderly shutdown would converge on the same validated recovery profile. Root restoration could be supported by a bounded `0x13` observation after `0x07`, with raw responses retained, but successful status readback would still not prove that all latched operations ended.

No generic `0x09`, `0x15`, `0x72`, power cycle or STOP substitution is authorized by this proposal. Each admitted byte needs its own evidence-based profile; unknown bytes would remain rejected. Cleanup attempts and retries would be bounded and evidence-bearing. Failed/missing/mismatched cleanup or an ambiguous ROOT probe would keep context possibly open, mark receiver integrity uncertain and block further active commands until operator recovery. Process crash, SIGKILL, power loss, failed GPIO or a permanently busy bus can defeat even a validated cleanup attempt.

### Observation, tests and enablement plan

The future implementation would reuse the monitor checkpoint and bounded research history before **every** entry, SEND and cleanup TX, including observations arriving before TX returns. It would preserve both `waveform_lsb` and `legacy_msb_short1`, monotonic timestamps, complete/incomplete frames, decoder reasons, event gaps and history overflow. Frames would be labeled by their temporal relationship to the local TX window; neither origin nor receiver acknowledgment would be invented. Missing responses after `0x17` would be recorded as missing evidence, not confirmation of entry.

Before enabling entry:

1. Archive attended, controlled normal entry/exit trials with a known physical recovery procedure, no active volume/CD hold, raw BUSY/DATA capture, both byte conventions and independent receiver observations. Validate an entry witness in a known non-motor transaction and `0x07` plus ROOT status readback; do not infer entry from RX silence alone. The existing MUTE lifecycle is evidence for that known transaction, not for arbitrary subsequent bytes.
2. Establish normal exit and interrupted/failed-operation recovery for each narrowly selected non-motor candidate. Keep VIDEO 1 / VIDEO 2 TX candidates unapproved until their state-dependent effects and required termination are known. No byte-range scan is proposed.
3. Implement deterministic fake-backend tests for token ownership across clients, entry/exit ordering, absolute expiry, missing heartbeat, stale tokens, every TX/waveform failure, recovery limits, monitor gaps, receiver-integrity loss, shutdown, and interleaved STATUS/CEC busy replies. Verify all raw `0x17` routes remain forbidden and that no test introduces a volume START as an entry probe.
4. Run existing service, CEC, Gateway and REPL regressions, then review the exact physical validation procedure and startup gate. Fake tests validate state handling, not the protocol assumptions in steps 1–2.

No transmitting context-session implementation or successful physical entry/exit tests were added. The preparation subset above has no transmitter interface; existing controls retain their hardware behavior outside an explicitly reserved preparation window.
