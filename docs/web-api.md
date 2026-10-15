# Web Remote and HTTP API v0.1

The Gateway remains **read-only by default**. Explicit production `--enable-mute` and `--enable-source` options independently gate MUTE and the six evidenced source selections (CD, TUNER, PHONO, VIDEO1, VIDEO2, TAPE1) through authenticated HTTPS and the systemd Unix socket. TAPE2 remains blocked as a source. A separate default-off `--enable-volume` option gates single-tap VOL+/VOL− steps ([below](#volume-step-phase-1)); hold and repeat are not exposed. `--enable-cd-playback` and `--enable-cd-seek` gate CD PLAY/PAUSE and STOP, and held FF/REW ([below](#cd-playback-and-held-seek)). `--enable-volume-hold`, `--enable-power` and `--enable-tape2-monitor` gate held VOL+/VOL−, POWER ON/OFF (and one-click POWER, which also needs `--enable-receiver-query`) and the provisional TAPE2 MONITOR toggle ([below](#web-remote-v2-controls)). Every gate is off by default. A separate default-off `--enable-cd-track` option gates discrete CD NEXT/PREVIOUS ([below](#cd-track-nextprevious-phase-a)). The checked-in systemd unit enables no control. See [MUTE deployment](mute-control.md) and [source selection evidence and rollout](source-selection.md) before opting in.

An independent default-off `--enable-receiver-query` gate permits one explicit authenticated `POST /api/v1/receiver/query` for live source/MONITOR synchronization. GET status and background polling remain passive. See [live receiver status](receiver-status.md) for the cache fields, uncertainty model, and manual rollout.

## Run locally

Prerequisite: Python 3 with the standard library. From the repository root:

```sh
python3 xs8_gateway.py
```

Open `http://127.0.0.1:8080/` on the Raspberry Pi. The page still loads when `xs8-service` is unavailable and shows the connection failure. Start `xs8-service` separately if live status is needed, and run the gateway as a local user permitted to open its socket. The service's default socket is `/tmp/kenwood-xs8.sock`. Use `--socket PATH` for a different local socket or `--port PORT` for a different port. By default, the gateway binds only to `127.0.0.1`.

## Temporary read-only LAN preview

To open the page in iPhone Safari on the same home Wi-Fi, find the Pi's specific LAN IPv4 address and start the gateway explicitly, for example:

```sh
python3 xs8_gateway.py --bind 192.168.1.20 --allow-insecure-readonly-lan
```

Replace `192.168.1.20` with the Pi's actual address, then open `http://192.168.1.20:8080/` on the iPhone. This mode is **unauthenticated, unencrypted HTTP**. Use it only as a temporary read-only preview on the home LAN. Do not forward the port through the router or expose it to the public internet. The gateway rejects wildcard addresses (`0.0.0.0`, `::`) and requires a literal IPv4 address; a non-loopback address requires the explicit opt-in flag. The flag is rejected with a loopback address to avoid a misleading startup configuration. All controls stay disabled and every POST returns HTTP 501 without accessing the XS8 service.

For authenticated HTTPS on the LAN, use the [nginx setup guide](https-lan.md) and the [socket-activated Gateway units](gateway-systemd.md). This route has no Gateway TCP listener.

For optional gateway autostart after reboot, see the [systemd unit guide](gateway-systemd.md). It starts only the read-only Gateway; `xs8-service` remains separate.

Offline tests (no GPIO or receiver access):

```sh
python3 -m unittest discover -s tests -p 'test_gateway.py'
```

## Implemented API

| Endpoint | Result |
| --- | --- |
| `GET /api/v1/status` | JSON with `gateway`, `xs8_service`, parsed service `status`, `receiver` and `cd_changer` (diagnostic CD changer presence, or `null` from a service without it; see [CD changer presence](cd-changer-presence.md)). Returns HTTP 503 when the service is unavailable or its reply is invalid. |
| `GET /api/v1/capabilities` | JSON classifications and enabled flags. MUTE and evidenced sources require their own gates, a session, and XS8 eligibility. |
| `GET /api/v1/session` | Returns 404 when every control gate is disabled; otherwise issues a secure session cookie and one-use CSRF token, plus per-control `*_available` flags including `cd_track_available` `volume_step_available`, `cd_playback_available` and `cd_seek_available`. |
| `POST /api/v1/mute` | Returns 501 with MUTE disabled. With the production gate, a valid request checks fresh XS8 eligibility and sends one `MUTE` to the local service. |
| `POST /api/v1/source` | Returns 501 with source disabled. With the production gate, accepts one allowlisted `{ "source": "CD" }` style JSON body, checks fresh XS8 eligibility, and dispatches at most one command, or, when leaving a freshly observed CD source, the source then `CD_STOP`; a verified selection then ends with one SYNC when the query gate is on ([CD auto-stop and final SYNC](source-selection.md)). TAPE2 returns 409 without dispatch. |
| `POST /api/v1/cd/track` | Returns 501 with the CD gate disabled. With `--enable-cd-track`, accepts exactly `{ "action": "NEXT" }` or `{ "action": "PREVIOUS" }`, checks fresh XS8 and CD eligibility, and dispatches at most one discrete `CD_TRACK_*` pair. |
| `POST /api/v1/cd/playback` | Returns 501 with `--enable-cd-playback` off. Otherwise accepts exactly `{ "action": "PLAY_PAUSE" }` or `{ "action": "STOP" }` and dispatches at most one single-frame key. |
| `POST /api/v1/cd/seek` | Returns 501 with `--enable-cd-seek` off. Otherwise accepts `START` (with `direction` `FORWARD`/`BACKWARD`), `HEARTBEAT` (with `hold_id`) or `RELEASE` (optional `hold_id`) for one held seek. |
| `POST /api/v1/volume/hold` | Returns 501 with `--enable-volume-hold` off. Otherwise `START` (with `direction` `UP`/`DOWN`), `HEARTBEAT` (with `hold_id`) or `RELEASE` for one connection-owned volume hold. |
| `POST /api/v1/power` | Returns 501 with `--enable-power` off. Otherwise exactly `{ "action": "ON" }` or `{ "action": "OFF" }` (one ROOT frame), or `{ "action": "TOGGLE", "expected": "ON" \| "OFF_INFERRED" }` ([one-click POWER](#one-click-power)). |
| `POST /api/v1/receiver/monitor` | Returns 501 with `--enable-tape2-monitor` off. Otherwise body `{}`; one provisional TAPE2 MONITOR toggle. |
| `POST /api/v1/volume/step` | Returns 501 with the volume gate disabled. With `--enable-volume`, accepts exactly `{ "direction": "UP" }` or `{ "direction": "DOWN" }`, checks fresh XS8 eligibility, and dispatches at most one atomic `VOLUME_STEP`. |

`status` preserves the service's `last_gpio_tx`, `last_waveform`, and `receiver_execution` fields. `TX_OK` and `MATCH` do not prove that the Kenwood executed a command. The gateway opens one short-lived Unix socket connection per status request. It does not modify XS8 state.

## Isolated offline MUTE test path

The legacy `--enable-dev-mute` flag remains rejected. `--enable-offline-mute` is a default-off test gate. It requires `--systemd-socket`, a verified nginx worker UID, an exact `--https-origin`, and an explicit non-default fake `--socket`. TCP and LAN-preview modes cannot enable it. Before sending `MUTE`, the Gateway requires a random `OFFLINE_TEST` challenge response from the fake service **on the same connection**. The real XS8 Service does not implement that response. A non-default pathname by itself is not proof that a service is fake; retain the challenge and never point test tooling at a live service.

Run isolated tests from the repository root without changing the deployed nginx or systemd units:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_gateway.py'
```

The suite creates temporary Gateway and fake XS8 Unix sockets, then closes them. It uses an nginx-like authenticated request model; on Linux it exercises real `SO_PEERCRED`. It does not inspect the deployed nginx instance, terminate the live Gateway, or contact `/tmp/kenwood-xs8.sock`. Review `sudo nginx -T` and independently test forged identity headers against an **isolated** nginx instance before treating header replacement as deployed evidence. Do not start an offline-gated Gateway on the live `/run/kenwood-gateway.sock`.

Both MUTE modes and production source selection use a 30-minute, Secure, HttpOnly, SameSite=Strict session cookie and a one-use CSRF token. They check exact HTTPS Origin, method, content type and body. Production controls additionally require the default real XS8 Service socket and a fresh eligible STATUS before one dispatch. Service TX replies are transmission evidence, never proof of receiver execution.

The XS8 Service retains final authorization, including BOOT_INHIBIT and FAULT_LATCHED. A process running as the nginx worker UID can reach the Gateway Unix socket directly; verify that account is not shared by unrelated local applications. HTTPS, Basic Auth, restricted socket permissions, peer UID validation, Origin and CSRF checks remain required for physical Web control.

## CD track NEXT/PREVIOUS (Phase A)

`--enable-cd-track` is independent of the MUTE, SOURCE and query gates and has the same startup requirements: `--systemd-socket`, a verified `--nginx-uid`, an exact `--https-origin`, and the default real XS8 Service socket. It cannot be combined with the offline fake MUTE mode. The XS8 Service remains the single GPIO owner and performs the [discrete PRESS → 100 ms → RELEASE pair](cd-transport-api.md); the Gateway adds no frame, timing or RELEASE logic of its own and never sends `CD_PRESS`, `CD_HEARTBEAT` or `CD_RELEASE`.

A request passes the existing verified-peer, Basic Auth identity, exact Origin, `X-Forwarded-Proto: https`, session cookie and one-use CSRF checks. The body must be at most 64 bytes of `application/json` with exactly one `action` key whose value is `NEXT` or `PREVIOUS`. Lower-case names, duplicate keys, extra keys, `PLAY_PAUSE`, `STOP` and SEEK actions return 400 without reading service STATUS. Only one CD request can be in progress across the whole Gateway; an overlapping one returns 409 `CD_BUSY` without reading STATUS. A request that arrives less than the configured interval after the **previous dispatch completed** (any outcome, including timeout) returns 429 `TOO_FAST` with `min_interval_ms`. Both refusals rotate the CSRF token and dispatch nothing. The interval is server-wide, so a second session or browser cannot bypass it, and NEXT and PREVIOUS share it. Requests refused before dispatch do not start it. See [CD track spacing](#cd-track-spacing-experimental).

Before dispatch the Gateway reads a fresh STATUS. It requires `state=IDLE`, a healthy monitor, no volume STOP obligation, no possibly open context, and the CD STATUS fields with `cd_state=IDLE` and `cd_release_owed=0`. A service without CD STATUS fields is ineligible. It then opens one socket connection, sends one `CD_TRACK_NEXT` or `CD_TRACK_PREVIOUS`, and waits up to 3 s for the single reply. No request is retried.

| HTTP | `result` | Meaning | `physical_check_required` |
|---|---|---|---|
| 200 | `SERVICE_TX_OK` | Exact `OK CD_TRACK_TX` with both frames `TX_OK`/`MATCH` and `cd_result=TX_VERIFIED`. Includes `action`, all four evidence fields, `cd_result` and `receiver_execution=UNVERIFIED`. **Not** proof of a track change. Also carries `csrf_token`: the session's next single-use token (see below). | — |
| 409 | `NOT_ELIGIBLE` | STATUS failed an eligibility check. Nothing dispatched. | — |
| 409 | `CD_BUSY` | Another Web CD request is in progress, or STATUS shows a CD pair or explicit hold active (for example from the REPL). Nothing dispatched. | — |
| 429 | `TOO_FAST` | Less than `min_interval_ms` since the previous dispatch completed. Nothing dispatched. | — |
| 409 | `CD_FAULT_LATCHED` | STATUS reports `cd_state=FAULT_LATCHED` or an idle owed RELEASE, or the service replied `ERR CD_FAULT_LATCHED`. | `true` |
| 409 | `REJECTED` | Any other service `ERR` refusal (`CD_TRACK_BUSY`, `SHUTDOWN`, `RESEARCH reason=…`, …). Pre-TX refusal; `error` carries the line. | `false` |
| 502 | `SERVICE_UNCERTAIN` | Exact `ERR CD_TRACK_UNCERTAIN` with `cd_result=PRESS_UNCERTAIN` or `RELEASE_UNCERTAIN`; evidence fields included. The service has latched a CD fault. | `true` |
| 502 | `INVALID_REPLY` / `DISCONNECTED` | Unparseable or mismatched TX report (wrong action, non-`MATCH` field in an `OK`, missing `receiver_execution=UNVERIFIED`, …) or the connection closed after sending. Delivery is uncertain. | `true` |
| 503 | `UNAVAILABLE` | STATUS or the command connection could not be opened. Nothing dispatched. | absent (STATUS) or `false` (command connection) |
| 504 | `TIMEOUT` | No reply within 3 s. The service may still complete or have completed the pair; it keeps its own RELEASE obligation. | `true` |

`physical_check_required: true` means the outcome may have moved the changer, so the operator should look at the changer and refresh STATUS before retrying. It never clears or bypasses a latched service fault. Recovery after `CD_FAULT_LATCHED` is the [operator procedure](cd-transport-api.md#failure-shutdown-and-operator-recovery): physically stop and inspect the changer, then restart the service with the existing explicit authorization. The Web Remote provides no fault-clear control.

`GET /api/v1/capabilities` reports `cd_transport` with `classification=unverified`, `available_actions=["NEXT","PREVIOUS"]`, `blocked_actions=["PLAY_PAUSE","STOP","SEEK_FORWARD","SEEK_BACKWARD"]`, `min_interval_ms` (the Gateway's configured interval) and `enabled` true only for a valid session with the gate on and a CD-eligible STATUS.

### Web Remote behavior

In the CD transport section, PREV and NEXT use this endpoint; PLAY / PAUSE, STOP, REW and FF use their own gates ([below](#cd-playback-and-held-seek)). PREV and NEXT stay disabled unless the session reports `cd_track_available`, capabilities list the action, and the latest STATUS shows `cd_state=IDLE` with no owed RELEASE. Each click consumes the one-use CSRF token. Buttons stay disabled while a request is in flight and until the follow-up refresh delivers a fresh token; taps in that window are dropped, never queued. The client also ignores taps less than `min_interval_ms` after its previous CD request completed, using the value from capabilities; a missing or out-of-range value falls back to 1000 ms. Results read `NEXT TX VERIFIED · TRACK CHANGE UNVERIFIED`; failures say `CHECK CHANGER` when `physical_check_required` is set and `NOTHING SENT` otherwise; a network error reads `CD RESULT UNKNOWN · CHECK CHANGER`. While STATUS reports a CD fault, the status line shows `CD FAULT LATCHED · STOP AND INSPECT CHANGER` in place of other results. Connection details include the CD state, last CD result and an owed-RELEASE marker. PREVIOUS may restart the current track rather than move back, as observed in the validated setup.

**One PREVIOUS tap vs two.** Each command is one separate `CD_TRACK_PREVIOUS` pair (`0xEC → 100 ms → 0x72`). In attended validation, a single PREVIOUS restarted the current track and two successive PREVIOUS commands moved to the preceding track. The changer decides between restart and moving back, so every result remains `TRACK CHANGE UNVERIFIED`.

**Double-PREV gesture.** A natural double tap is shorter than one command (about 331 ms of XS8 time; Web readiness was measured at 634 ms). So the browser accepts **one** explicit second PREV tap made within **800 ms** of the first PREV tap while that first operation is still running, cooling down or refreshing its token. During that time PREV stays clickable rather than `disabled`, so the tap reaches the page. The button gets a pending outline and the status line reads `PREV ×2 · SECOND PREV PENDING`. The browser sends the pending PREV once, through the normal request path, and only when all of these hold:

- the first PREV returned 200 `SERVICE_TX_OK`;
- at least `min_interval_ms` (default 250; immediately at 0) has passed since it completed;
- a fresh CSRF token is available: normally the one returned in the first PREV's 200 response, otherwise one from a refresh;
- no refresh that arrived first showed lost eligibility (STATUS or capabilities) or a lost connection;
- no CD fault is known and the last observed source has not changed since the first tap.

The Gateway still runs all of its own checks (authorization, CSRF, a fresh STATUS read, its interval and the global lock) on that request.

The pending tap is **cancelled, never retried**, if any of these happens:

- the first PREV fails, is refused (409, 429), is uncertain (502), times out (504) or loses its connection. A timeout or network error still reads `CHECK CHANGER`, never "nothing sent";
- STATUS, eligibility, connection or the observed source changes;
- another control is used;
- the page is hidden or unloaded.

**Second-PREV token and latency.** The Gateway rotates the CSRF token before every dispatch, so the token the first PREV used is already invalid. A successful CD response (200 `SERVICE_TX_OK` with `cd_result=TX_VERIFIED`) returns the rotated, still single-use successor as `csrf_token`. The browser keeps it only for the pending second PREV and uses it once. The second POST therefore no longer waits for the three-request display refresh (status, session, capabilities). It waits only for the first response and `min_interval_ms`. That refresh still runs for the display; it is superseded if the second PREV starts first, and a late result cannot overwrite a newer token or re-enable controls after an uncertain result. Without a returned token (an older Gateway), the browser waits for the refresh as before. The Gateway still reads STATUS before every transmission and still enforces the lock and the interval.

Security: the returned token is the same value `GET /api/v1/session` would give the same authenticated, same-origin session. It goes only in a `Cache-Control: no-store` JSON body after the Origin, identity, cookie and CSRF checks, and is never returned on errors or refusals. It is never written to `Server-Timing` or the journal line. The server handles one request at a time, so checking and rotating the token cannot interleave with another request, and the previous token is rejected (403) on replay.

Limitation: with the fast path, a source change or a new fault after the first PREV can stop the second only if a refresh shows it before dispatch. At `min_interval_ms: 0` the second normally goes out before any refresh, so only the Gateway's STATUS checks apply (IDLE, monitor, no STOP or context, CD idle with no owed release, no fault). The Gateway does not check the selected source. STATUS does include the receiver's last *passively observed* source (`receiver_source`, with its timestamp, evidence and `receiver_integrity`). But it changes only when the monitor decodes a receiver source broadcast before the Gateway reads STATUS. A change may not yet be broadcast or decoded within about 100 ms, may never be observed, or may leave the value `UNKNOWN` or stale. So it cannot reliably show that the input is still CD, and the CD preflight deliberately does not use it. This is not new to the double-PREV path: a single PREV or NEXT tap is accepted whatever the observed source, because CD eligibility has never depended on it. A PREV sent after the source changed is therefore a possible unwanted changer command. Its effect with a non-CD input selected has not been characterized; it is expected to act on the changer as in ROOT, but that is not established. Avoid switching the receiver source while a double-PREV gesture is in progress.

The pending tap exists only in page memory, so nothing replays after a reload or reconnect. A third or later tap during the gesture never adds a command; the status line shows `EXTRA TAP IGNORED`. After the second PREV finishes, whether it succeeded or failed, the gesture ends only after **800 ms with no further tap**. PREV stays clickable and NEXT stays disabled until then, and the second PREV's result stays visible. A continuing burst therefore never starts a third command. Without a second tap, a tap more than 800 ms after the first is a separate action. NEXT is never queued, and there is no automatic double-press, hold-to-repeat or SEEK. The CD timing row adds `prev queued · extra · cancelled` counts once a gesture has been used.

### CD track spacing (experimental)

`--cd-track-interval-ms` sets the spacing from one **completed** CD track dispatch to the next accepted one. It requires `--enable-cd-track`, accepts **200–1000** or **0**, and defaults to **250**. The Gateway reports it as `min_interval_ms`, and the browser uses the same value. The Gateway prints it in the startup mode, for example `CD TRACK 250 ms`. Use `1000` to restore the Phase A spacing.

The value is an experimental setting for attended testing. It is not an established safe minimum.

- **Service:** separate commands cannot overlap. `CD_TRACK_*` runs synchronously and replies only after RELEASE. A concurrent command gets `CD_TRACK_BUSY`, and preflight refuses any fault or owed RELEASE. The service imposes no spacing after RELEASE, so the remaining question concerns the changer, not software state.
- **Evidence:** one research chain recorded as trial `id=34`: `EC → 72 → EC → 72` at a requested 100 ms gap (about 104–109 ms measured across that session, not per trial). The second, separate press moved to the preceding track. This is operator testimony from a single trial, without archived captures. It does not show that shorter gaps fail or that 100 ms is always enough.
- **Margins:** the 100 ms PRESS → RELEASE gap inside one pair says nothing about spacing between separate pairs. The default keeps 2.5× the only observed working RELEASE → PRESS gap, and the lower bound keeps 2×. On the Web path, the real spacing also includes the browser's token refresh, the Gateway's fresh STATUS read and service preflight, so it is always longer than the setting.
- **Unknown:** how long after a restart the changer still treats PREVIOUS as "move back". If two taps at 250 ms behave like two restarts, the cause is that changer threshold or slow UI round trips, not spacing that is too short. Record it and do not lower the bound.
- **Zero cooldown (`0`, experimental):** a two-way setting for attended diagnosis of the double-PREV gap. The Gateway reports `min_interval_ms: 0`, and the browser treats 0 as a real value, not as missing (missing or invalid values still fall back to 1000 ms). Neither the Gateway nor the browser adds any wait after a command completes.
  Everything else still applies:
  - one CD request at a time across the Gateway (409 `CD_BUSY`);
  - a fresh STATUS read before every command;
  - CSRF checks and token rotation;
  - strict reply parsing, with no retries;
  - the double-PREV gesture and its cancellation rules;
  - `sincecd` timing.

  Zero cooldown does **not** mean zero delay between XS8 frames. The next PRESS still waits for the previous HTTP response, a token refresh, the Gateway's STATUS read, the service's socket loop, its preflight checks and the bus-free check. Only the artificial interval is removed. It is not an evidence-based safe minimum and is not a recommended default.

### Rollout and rollback

Not performed by this change. Follow the [source-selection rollout pattern](source-selection.md): review `systemctl cat kenwood-xs-gateway.service`, add **only** `--enable-cd-track` (and, for the attended spacing trial, optionally an explicit `--cd-track-interval-ms`) to the existing Gateway drop-in while preserving `--systemd-socket`, `--nginx-uid`, `--https-origin` and any existing gates, run `systemd-analyze verify`, and stop only the Gateway so the next authenticated request activates it. The deployed XS8 Service must be a build that implements `CD_TRACK_*` and the CD STATUS fields. Attend the first trials with the changer visible and physical recovery available. For rollback, remove the flag and confirm `/api/v1/cd/track` returns 501 and PREV/NEXT are disabled.

Offline tests: `python3 -m unittest tests.test_gateway` (class `CdTrackGatewayTest`), `node tests/test_web_cd.js`, `node tests/test_web_cd_sequence.js` (consecutive fresh-token actions and duplicate browser events) `node tests/test_web_cd_rapid.js` (completion-based spacing, overlap and interval fallback), `node tests/test_web_cd_timing.js` and `node tests/test_web_cd_prev_gesture.js` (double-PREV gesture, cooldown, cancellation and no retry). They use fake services and establish Gateway routing, parsing and refusal behavior only.

## CD playback and held seek

Two further default-off gates complete the CD transport. Each has the same startup requirements as `--enable-cd-track`: `--systemd-socket`, a verified `--nginx-uid`, an exact `--https-origin`, the real XS8 Service socket, and never the offline MUTE mode. The startup mode prints `CD PLAYBACK` or `CD SEEK`. Capabilities gain `cd_playback` and `cd_seek` operations, and `cd_transport.blocked_actions` lists only what the configured gates do not expose. Existing fields are unchanged.

### PLAY/PAUSE and STOP

`--enable-cd-playback` enables `POST /api/v1/cd/playback`, which follows the CD track conventions exactly:
- Origin, session and a single-use CSRF token;
- the server-wide CD lock and the same completion-based spacing as PREV/NEXT;
- a fresh STATUS that must show an idle CD;
- one `CD_PLAY_PAUSE` or `CD_STOP` dispatch, never retried, with strict reply parsing;
- `physical_check_required` on anything but a clean refusal.

A verified `PLAY_PAUSE` reply carries `"mapping": "PROVISIONAL"`, because `0xBC` is a historical mapping not yet physically verified ([service API](cd-transport-api.md#single-frame-keys-play-playpause-and-stop)). `PLAY` (`0x41`) is not exposed.

### Held FF/REW

`--enable-cd-seek` enables `POST /api/v1/cd/seek`. It uses the service's **connection-owned** hold: `KEY_DOWN SEEK_FORWARD|SEEK_BACKWARD` and `KEY_UP` ([service API](cd-transport-api.md#connection-owned-seek-key_down--key_up)). It is not replaced by discrete bursts, and the service's token API (`CD_PRESS`/`CD_HEARTBEAT`/`CD_RELEASE`) is not used by the Gateway.

- **One persistent service connection per active seek.** The Gateway keeps it in a server-wide registry, together with the session that started it. The service releases the hold as soon as that connection closes, so **closing it is the Gateway's emergency release**. The browser receives only a random `hold_id`, which is never logged.
- **START** takes the CD lock and spacing and reads STATUS. If a registry entry exists but STATUS already shows an idle CD (the service ended the hold), that dead connection is closed. START refuses with `CD_BUSY`, `CD_FAULT_LATCHED` or `NOT_ELIGIBLE`; otherwise it opens a new connection and sends exactly one `KEY_DOWN`, then strictly validates the reply.
  - A verified press keeps the connection and answers `SEEK_STARTED` with `hold_id`, `heartbeat_ms` 500, `lease_ms` 1200 (the service's inactivity limit) and `max_hold_ms` 10000.
  - Any other reply, an error or a timeout closes the connection and registers nothing. Closing makes the service release any press it did make.
  - If the `SEEK_STARTED` reply cannot be written to the browser, the connection is closed at once.
- **HEARTBEAT** must name the session's current `hold_id`; otherwise it is refused without touching the connection. It sends the same `KEY_DOWN` on the owning connection and accepts only `OK KEY_ALIVE`, which renews the service's inactivity limit **without a new XS8 frame**.
  - A late keepalive after the service's own `INACTIVITY` or `MAX_HOLD` release gets `KEY_EXPIRED`, never a new press. The Gateway closes the connection and reports `NOT_ACTIVE` with `cd_last_result`.
  - At 10 s the Gateway stops forwarding renewals and closes the connection (`reason: MAX_HOLD`).
  - Any error, timeout or unexpected reply closes the connection and is reported with `physical_check_required`.
- **RELEASE** is stop-only. It sends `KEY_UP` on the owning connection, reads the reply, and **always closes** the connection.
  - Without `hold_id` it releases the session's current hold, so a browser that lost the START response can still stop it.
  - With a stale `hold_id` it returns `NOT_OWNER` and touches nothing, so it can never stop a newer hold.
  - With no hold it returns `NOT_ACTIVE` without contacting the service; `KEY_UP_NONE` from the service is also reported as `NOT_ACTIVE` with `cd_last_result`.
  - `KEY_UP_UNCERTAIN` reports `SERVICE_UNCERTAIN`; the service has latched a CD fault and kept its obligation. A timed-out, disconnected or malformed reply is uncertain; the close still makes the service release.
- **Cleanup without threads.** The server's `service_actions()` hook, run by `serve_forever` about every 0.5 s, closes the owning connection when the browser has been silent for 1.5 s, the session has expired or been removed, or the hold reaches 10 s. Gateway shutdown (`server_close`) closes it too, and a Gateway crash closes it in the kernel. Nothing is ever reconnected or resent.
- **CSRF.** Every seek POST consumes its token. Every reply that passed the CSRF check carries the single-use successor, so RELEASE never waits for a refresh. After a lost response the browser recovers the current token with `GET /api/v1/session`. A malformed body gets 400 before the token is consumed.
- **Backstops.** Independently of the Gateway and browser, the service releases on connection close, after 1200 ms without `KEY_DOWN`, and at its 10 s maximum.

### Web Remote behavior

The CD grid shows PREV, PLAY / PAUSE and NEXT, then REW, STOP and FF. PLAY / PAUSE and STOP are click controls with the same enablement, in-flight, spacing, failure and no-retry rules as PREV/NEXT.

FF and REW are **press-and-hold** controls driven by pointer events. They have no click or keyboard action.
- **Press:** `pointerdown` with the primary button captures the pointer and sends START. Further pointerdowns, and the other seek button, are ignored while a seek exists. Every other control is disabled; the held button stays enabled so its release events arrive.
- **Hold:** one HEARTBEAT is sent every `heartbeat_ms` after the previous one completes, using the returned successor token.
- **Release:** any of these ends the hold:
  - `pointerup`, `pointercancel` or `lostpointercapture` on the button;
  - window `blur`, or a window-level `pointerup`/`pointercancel`;
  - `visibilitychange` to hidden, or `pagehide`.
  
  Cancellation is idempotent: heartbeats stop immediately and exactly one RELEASE is sent.
- **Release before START answers:** the cancellation is recorded, and RELEASE is sent as soon as START answers. A delayed START therefore never leaves a seek running.
- **Unknown START outcome** (no reply, or a 5xx such as `SERVICE_UNCERTAIN`): the browser sends one session-scoped RELEASE and never another START. A clean refusal (`CD_BUSY`, `TOO_FAST`, `NOT_ELIGIBLE`, `REJECTED`) releases nothing.
- **Lost heartbeat response, consumed token or Gateway timeout:** the hold continues, and the next heartbeat recovers a token by GET. If renewals keep failing, the service lease ends the hold. A `NOT_ACTIVE` reply ends the loop.
- **Client maximum:** the browser also releases on its own at `max_hold_ms` (10 s), even if the pointer stays down.
- **Results** read `FF RELEASE TX VERIFIED · SEEK RESULT UNVERIFIED`, `FF ENDED · LEASE_EXPIRED`, `CD FAULT LATCHED · STOP AND INSPECT CHANGER`, or `FF RELEASE UNKNOWN · LEASE ENDS SEEK WITHIN 2 s · CHECK CHANGER`.

Offline tests: `python3 -m unittest tests.test_gateway` (classes `CdPlaybackGatewayTest`, `CdSeekGatewayTest`, and `RealServiceSeekTest`, which drives the real service socket loop with its fake GPIO backend after `make test`), `node tests/test_web_cd_seek.js`, and the service's `make test`. They establish routing, parsing, connection ownership and timing logic only, not physical behavior.

### Rollout and rollback

Rollout follows the [CD track pattern](#rollout-and-rollback), with `--enable-cd-playback` and/or `--enable-cd-seek`. The XS8 Service must be a build with `CD_PLAY_PAUSE`/`CD_STOP` and the 10 s CD hold maximum. To roll back, remove the flags and confirm that both endpoints return 501 and the four buttons are disabled.

## Web Remote v2 controls

All three gates below share the active-control startup requirements of `--enable-cd-track` and are printed in the startup mode (`VOLUME HOLD`, `POWER`, `TAPE2 MONITOR`). Capabilities gain `volume_hold`, `power` and `tape2_monitor`; the session gains `volume_hold_available`, `power_available` and `tape2_monitor_available`. VIDEO1 and VIDEO2 need no new gate: they join `available_sources` under the existing `--enable-source` ([source selection](source-selection.md)); TAPE2 stays a blocked source.

### Volume hold

`POST /api/v1/volume/hold` reuses the CD seek handler and its single hold registry (the service allows one hold of any kind) with the service's connection-owned volume hold: `KEY_DOWN VOLUME_UP|VOLUME_DOWN` starts `0x17 → 0xF9/0xE9`; a repeated `KEY_DOWN` on the owning connection renews it without a frame; `KEY_UP`, or closing the connection, sends STOP `0x09` then exit `0x07` ([service API](volume-step-api.md#connection-owned-volume-hold)). START uses the volume lock and 250 ms spacing shared with `VOLUME_STEP`, and refuses with `VOLUME_BUSY` (any volume hold, including CEC's), `CD_BUSY`, `FAULT_LATCHED`, `CD_FAULT_LATCHED` or `NOT_ELIGIBLE`. A successful START answers `HOLD_STARTED` with `hold_id`, `heartbeat_ms` 500, `lease_ms` 1200 (the service's inactivity limit) and `max_hold_ms` 20000: one absolute 20 s MAX_HOLD shared with CEC. The service's `--max-hold-ms` (100–20000 ms, default 20000) remains authoritative; the Gateway closes the owning connection at 20 s as an independent backstop, and the browser releases at the capability's `max_hold_ms` (never trusted above 20000). A late keepalive after the service watchdog gets `KEY_EXPIRED` and is reported `NOT_ACTIVE`, never a new press. Session, `hold_id`, CSRF, cleanup and no-retry rules are exactly those of [held FF/REW](#held-ffrew); a CD RELEASE never ends a volume hold and vice versa. Single-click `POST /api/v1/volume/step` is unchanged.

**Web gesture.** With `volume_hold` available, a VOL+/VOL− `pointerdown` starts a `hold_delay_ms` (400 ms) timer. Released earlier, the gesture is the existing click: one `VOLUME_STEP`. Still held when the timer fires, it becomes a hold, and that gesture's click is suppressed until its pointer is released, so **one gesture never produces both a step and a hold**, even when the hold ended on its own before the finger lifted. Release, cancellation, blur, hiding, races and unknown outcomes follow the FF/REW rules. Without the capability, pointer events do nothing and the click still steps.

### POWER

`--enable-power` enables `POST /api/v1/power` with exactly `{ "action": "ON" }` or `{ "action": "OFF" }`. The service sends one ROOT frame: `0x52` ON or `0x62` OFF, both physically verified; neither is assumed idempotent, and a verified TX still does not confirm execution. CONTEXT `0x39` is never used. The service refuses POWER during any volume or CD hold, any STOP, CONTEXT or CD release obligation, a fault, an open research CONTEXT, an unhealthy monitor, BOOT_INHIBIT or shutdown; the Gateway also requires an eligible STATUS with CD fields, a server-wide lock and 2 s spacing. An uncertain frame is never retried and latches no fault (it leaves no obligation). The Web shows one round red POWER button at the top right of the panel. Without `--enable-power` the button is visibly disabled and the chooser never opens. Neither the standby LED nor the switched outlets indicate power.

### One-click POWER

`{ "action": "TOGGLE", "expected": "ON" | "OFF_INFERRED" }` needs `--enable-power` and `--enable-receiver-query` (`toggle_available` in the power capability; otherwise `409 TOGGLE_UNAVAILABLE`). `expected` is the state the browser showed; it only guards and never chooses the command. Under the POWER spacing lock and every other control lock (source, CD, volume, MONITOR), after the usual CSRF, session and STATUS gates, the Gateway observes the receiver afresh:

| Observation (`QUERY_STATUS SETTLE`) | Decision | Sent |
|---|---|---|
| A recognised two-frame source/MONITOR reply | `ON` | one `POWER_OFF` |
| A clean `NO_REPLY`, then, at least 1 s later, a second clean `NO_REPLY` | `OFF_INFERRED` | one `POWER_ON` |
| Anything else: `PARTIAL`, `EXTRA`, `EVENT_GAP`, `PREEMPTED`, `BUS_ACTIVE`, `SESSION_BUSY`, `TX_UNCERTAIN`, an unrecognised or invalid reply, a refusal, a timeout or lost reply, or a reply after one silence | `UNKNOWN` | nothing: `409 POWER_STATE_UNKNOWN` |

If the decision differs from `expected`, nothing is sent (`409 STATE_CHANGED`). Immediately before the POWER frame the Gateway reads STATUS again and refuses, sending nothing, if the service is unavailable (`503`), POWER is no longer eligible (`VOLUME_BUSY`, `CD_BUSY`, a fault, `NOT_ELIGIBLE`) or `last_byte` is no longer the query's `0x13` (`INTERVENING_TX`, another client transmitted). Every refusal carries `power_sent: false`, `physical_check_required: false` and `decision` (`state` and each observation's `result`/`reason`). A sent frame is reported like an explicit one, plus `sent`, `decision` and `power_sent: true`; an uncertain or lost POWER reply requires a physical check and is never retried. Worst case about 11 s (two STATUS reads, two 3 s queries, 1 s gap, one 2 s POWER reply); the browser allows 15 s and the nginx example 14 s.

**Not globally atomic.** The Gateway's locks serialise only Gateway requests. Between the last observation and the POWER frame, a direct XS8 Service client (the REPL or research tools, or another local socket user) can still transmit, including `POWER_ON`/`POWER_OFF`, and the receiver's front panel or IR remote can change power at any moment. The `INTERVENING_TX` check narrows the window for socket clients to the gap between the second STATUS read and the POWER frame but cannot close it; nothing can exclude the physical controls. The CEC client sends only volume and MUTE: it can cause refusals (`PREEMPTED`, `BUS_ACTIVE`, `VOLUME_BUSY`), never a different decision. Because neither POWER command's idempotence is established, a decision made just before such a change may send a command whose effect is unknown; avoid using direct service tools while the Web Remote is in use.

**Power observation.** `GET /api/v1/status` includes a diagnostic `power` object (`value` `ON` | `OFF_INFERRED` | `UNKNOWN`, `observed_mono_ns`, `age_ms`, `evidence`, `freshness` `FRESH` (≤ 30 s) | `STALE` | `UNKNOWN`). The Gateway keeps it in memory (a restart forgets it) and changes it only from its own queries: TOGGLE, automatic SYNC and manual SYNC. A recognised reply is `ON` (`QUERY`). Two clean settled `NO_REPLY` results 1–10 s apart are `OFF_INFERRED` (`QUERY_SILENCE`); one alone is `UNKNOWN` (`SINGLE_SILENCE`), as is a plain manual-SYNC silence (`UNSETTLED_SILENCE`); other faults leave the value to age. Any POWER transmission, explicit or toggled, verified or not, sets `UNKNOWN` (`POWER_TX`) before the frame; TX evidence never sets it.

**Web.** With a fresh `ON` (green ring) or `OFF_INFERRED` (dashed ring) and TOGGLE available, one click on POWER sends TOGGLE with that state. When the state is unknown or stale, or TOGGLE is unavailable, the click opens the explicit ON / OFF chooser, which sends exactly the chosen command as before. A refused toggle (`power_sent: false`) reads `POWER TOGGLE … · NOTHING SENT · CHOOSE ON OR OFF` and opens the chooser; nothing is retried. The displayed state changes only from a later `power` observation, never from a reply. MUTE stays a neutral button in the Volume column: it is a toggle without a mute-state readback, so the Web never shows it as muted.

### TAPE2 MONITOR (provisional)

`--enable-tape2-monitor` enables `POST /api/v1/receiver/monitor` (body `{}`), one `TAPE2_MONITOR_TOGGLE`: the service's atomic CONTEXT `0x17 → 0x48 → 0x07`, with the same entry, exit, fault and refusal rules as VIDEO1/VIDEO2. `0x48` in CONTEXT is a **historical** MONITOR toggle, not yet physically validated; every reply carries `"mapping": "PROVISIONAL"`, with a server-wide lock and 1 s spacing.

A verified MONITOR toggle ends with one SYNC when `--enable-receiver-query` is on: `TAPE2_MONITOR_TOGGLE → QUERY_STATUS`, holding the MONITOR, CD, volume and POWER locks throughout. The reply's `sync` result is separate from the toggle's; a failed SYNC never repeats the toggle, and the indicator still shows only a fresh, healthy observation ([final SYNC](source-selection.md)).

The TAPE2 button is the MONITOR control and indicator. Its background comes only from the passive receiver-state tracking (`receiver.monitor`, the ROOT `0x13` second frame or broadcasts: `0xD7` ON, `0xE7` OFF in waveform_lsb): **green** with the label `MONITOR ON` for a fresh (≤ 30 s), healthy, confirmed ON; unlit with the label `MONITOR OFF` for a fresh, confirmed OFF; **neutral** (unlit, label `MONITOR`) for UNKNOWN, LAST_KNOWN (stale), uncertain integrity or a lost connection. OFF has no fill of its own, so the button does not flip from a dark fill to light when a report ages past 30 s; initial load, SYNC and polling all use this one rule. A successful toggle TX never changes it; only a later observed report does.

Offline tests: `python3 -m unittest tests.test_gateway` (classes `RemoteV2GatewayTest` and `RealServiceVolumeHoldTest`) and `node tests/test_web_remote_v2.js`.

## Volume step (phase 1)

`POST /api/v1/volume/step` exposes the service's [atomic `VOLUME_STEP`](volume-step-api.md): one request is one bounded `0x17 → 0xF9/0xE9 → 0x09 → 0x07` movement, with STOP, ROOT exit and fault latching owned by the XS8 Service. The Gateway adds no frame, timing or STOP logic and never sends `VOLUME_PRESS` or `VOLUME_RELEASE`, so it never holds volume and cannot take a hold from the CEC client.

`--enable-volume` is default-off and independent of the other gates, with the same startup requirements as `--enable-cd-track`: `--systemd-socket`, a verified `--nginx-uid`, an exact `--https-origin` and the default real XS8 Service socket. It cannot be combined with the offline fake MUTE mode, and the Gateway prints `VOLUME STEP` in its startup mode.

Requests follow the CD track conventions: Origin, `X-Forwarded-Proto`, authenticated session and a single-use CSRF token, rotated before dispatch. The Gateway then reads a fresh STATUS and requires `state=IDLE`, a healthy monitor, `stop_owed=0`, `context_may_be_open=0` and the CD STATUS fields with `cd_state=IDLE` and `cd_release_owed=0`. Refusals before dispatch return 409 without contacting the service further:

| `result` | Cause | `physical_check_required` |
| --- | --- | --- |
| `VOLUME_BUSY` | a volume hold (CEC) is active, or another volume request is in flight | — |
| `CD_BUSY` | a CD pair or hold is active | — |
| `FAULT_LATCHED` / `CD_FAULT_LATCHED` | a latched fault | `true` |
| `NOT_ELIGIBLE` | any other ineligible STATUS, including a service without CD fields | — |

A step less than **250 ms** (`min_interval_ms`) after the previous volume dispatch **completed**, whatever its outcome, returns 429 `TOO_FAST`. The spacing is server-wide and separate from CD spacing. One socket connection carries one `VOLUME_STEP` with a 3 s reply timeout; **no request is ever retried**.

Responses keep `receiver_execution=UNVERIFIED`:
- **200 `SERVICE_TX_OK`:** `direction`, `volume_result=TX_VERIFIED`, the service's `state`, `stop_owed` and `context_may_be_open`, and a single-use successor `csrf_token`, as for CD track.
- **502 `SERVICE_UNCERTAIN`:** `volume_result` is `UNCERTAIN` (a frame or cleanup was unverified and the service latched a fault) or `CANCELLED` (shutdown during the step), with the service's state fields.
- **409 `REJECTED`:** a clean service refusal such as `ERR VOLUME_BUSY`; nothing was sent.
- **504 `TIMEOUT`, 502 `DISCONNECTED` or `INVALID_REPLY`:** delivery or outcome is unknown.
- **503 `UNAVAILABLE`:** the service could not be reached.

Every dispatch failure carries `direction` and `physical_check_required`, which is `false` only for `REJECTED` and `UNAVAILABLE`. No failure or refusal returns a token. The Gateway journals one `VOLUME_STEP direction=… code=… result=…` line per request, with timings and without tokens.

### Web Remote behavior

The existing rocker's `+` and `−` buttons are enabled only when the session reports `volume_step_available`, capabilities list the direction under `volume_step.available_directions`, and the latest STATUS is IDLE with an idle CD. Each click sends exactly one request; both buttons, like every other control, stay disabled while it is in flight and until the follow-up refresh supplies a fresh token. Clicks in that window, or within `min_interval_ms` of the previous completed step, are dropped, never queued. There is no pointer hold, key repeat or automatic repeat. Results read `VOL+ TX VERIFIED · VOLUME CHANGE UNVERIFIED`; failures read `CHECK RECEIVER` when `physical_check_required` is set (`· RESTART REQUIRED` for a latched fault) and `NOTHING SENT` otherwise; a network error reads `VOLUME RESULT UNKNOWN · CHECK RECEIVER`. Nothing is retried.

Offline tests: `python3 -m unittest tests.test_gateway` (class `VolumeStepGatewayTest`) and `node tests/test_web_volume.js`. They use fake services and establish Gateway routing, parsing and refusal behavior only.

Rollout follows the [CD track pattern](#rollout-and-rollback) with `--enable-volume` in place of `--enable-cd-track`; the XS8 Service must be a build with `VOLUME_STEP`. For rollback, remove the flag and confirm `/api/v1/volume/step` returns 501 and the rocker is disabled.

## Future control API design (not implemented)

The following paths return HTTP 501 and do not access the XS8 service:

| Endpoint | Proposed request | Safety condition before implementation |
| --- | --- | --- |
| `POST /api/v1/cd/transport` | None; superseded by `/api/v1/cd/track`, `/api/v1/cd/playback` and `/api/v1/cd/seek` | Kept as a 501 placeholder so older clients fail closed. |

No new XS8 byte values are assigned by this API design. Future iOS clients can use the same versioned JSON endpoints and show capabilities before presenting enabled controls.

## LAN access plan

The direct LAN mode above is a temporary read-only exception. Before adding active controls, use the authenticated HTTPS reverse proxy and socket-activated Gateway, restrict the listener to the intended LAN, and protect every future mutating endpoint against cross-site requests. Browser clients can use a secure same-origin session; a future SwiftUI app can store its credential in Keychain. Do not forward this API to the public internet. Gateway socket permissions and XS8 Service socket permissions are separate local access boundaries.
