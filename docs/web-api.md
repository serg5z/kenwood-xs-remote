# Web Remote and HTTP API v0.1

The Gateway remains **read-only by default**. Explicit production `--enable-mute` and `--enable-source` options independently gate MUTE and the four evidenced source commands through authenticated HTTPS and the systemd Unix socket. TV1, TV2 and TAPE2 remain blocked pending controlled TX validation; volume remains disabled. A separate default-off `--enable-cd-track` option gates discrete CD NEXT/PREVIOUS ([below](#cd-track-nextprevious-phase-a)); CD PLAY/PAUSE, STOP and held SEEK are not exposed. The checked-in systemd unit enables no control. See [MUTE deployment](mute-control.md) and [source selection evidence and rollout](source-selection.md) before opting in.

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
| `GET /api/v1/status` | JSON with `gateway`, `xs8_service`, and parsed service `status`. Returns HTTP 503 when the service is unavailable or its reply is invalid. |
| `GET /api/v1/capabilities` | JSON classifications and enabled flags. MUTE and evidenced sources require their own gates, a session, and XS8 eligibility. |
| `GET /api/v1/session` | Returns 404 when every control gate is disabled; otherwise issues a secure session cookie and one-use CSRF token, plus per-control `*_available` flags including `cd_track_available`. |
| `POST /api/v1/mute` | Returns 501 with MUTE disabled. With the production gate, a valid request checks fresh XS8 eligibility and sends one `MUTE` to the local service. |
| `POST /api/v1/source` | Returns 501 with source disabled. With the production gate, accepts one allowlisted `{ "source": "CD" }` style JSON body, checks fresh XS8 eligibility, and dispatches at most one command. TV1, TV2 and TAPE2 return 409 without dispatch. |
| `POST /api/v1/cd/track` | Returns 501 with the CD gate disabled. With `--enable-cd-track`, accepts exactly `{ "action": "NEXT" }` or `{ "action": "PREVIOUS" }`, checks fresh XS8 and CD eligibility, and dispatches at most one discrete `CD_TRACK_*` pair. |

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
| 200 | `SERVICE_TX_OK` | Exact `OK CD_TRACK_TX` with both frames `TX_OK`/`MATCH` and `cd_result=TX_VERIFIED`. Includes `action`, all four evidence fields, `cd_result` and `receiver_execution=UNVERIFIED`. **Not** proof of a track change. | — |
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

The CD transport section shows only PREV and NEXT; PLAY/PAUSE and STOP were removed. Both buttons stay disabled unless the session reports `cd_track_available`, capabilities list the action, and the latest STATUS shows `cd_state=IDLE` with no owed RELEASE. Each click consumes the one-use CSRF token. Buttons stay disabled while a request is in flight and until the follow-up refresh delivers a fresh token; taps in that window are dropped, never queued. The client also ignores taps less than `min_interval_ms` after its previous CD request completed, using the value from capabilities; a missing or out-of-range value falls back to 1000 ms. Results read `NEXT TX VERIFIED · TRACK CHANGE UNVERIFIED`; failures say `CHECK CHANGER` when `physical_check_required` is set and `NOTHING SENT` otherwise; a network error reads `CD RESULT UNKNOWN · CHECK CHANGER`. While STATUS reports a CD fault, the status line shows `CD FAULT LATCHED · STOP AND INSPECT CHANGER` in place of other results. Connection details include the CD state, last CD result and an owed-RELEASE marker. PREVIOUS may restart the current track rather than move back, as observed in the validated setup.

**One PREVIOUS tap vs two.** Each command is one separate `CD_TRACK_PREVIOUS` pair (`0xEC → 100 ms → 0x72`). In attended validation, a single PREVIOUS restarted the current track and two successive PREVIOUS commands moved to the preceding track. The changer decides between restart and moving back, so every result remains `TRACK CHANGE UNVERIFIED`.

**Double-PREV gesture.** A natural double tap is shorter than one command (about 331 ms of XS8 time; Web readiness was measured at 634 ms). So the browser accepts **one** explicit second PREV tap made within **800 ms** of the first PREV tap while that first operation is still running, cooling down or refreshing its token. During that time PREV stays clickable rather than `disabled`, so the tap reaches the page. The button gets a pending outline and the status line reads `PREV ×2 · SECOND PREV PENDING`. The browser sends the pending PREV once, through the normal request path, and only when all of these hold:

- the first PREV returned 200 `SERVICE_TX_OK`;
- at least `min_interval_ms` (250) has passed since it completed;
- a refresh delivered a fresh CSRF token;
- STATUS and capabilities still allow PREVIOUS;
- the observed source has not changed since the first tap.

The Gateway still runs all of its own checks (authorization, CSRF, a fresh STATUS read, its interval and the global lock) on that request.

The pending tap is **cancelled, never retried**, if any of these happens:

- the first PREV fails, is refused (409, 429), is uncertain (502), times out (504) or loses its connection. A timeout or network error still reads `CHECK CHANGER`, never "nothing sent";
- STATUS, eligibility, connection or the observed source changes;
- another control is used;
- the page is hidden or unloaded.

The pending tap exists only in page memory, so nothing replays after a reload or reconnect. A third or later tap during the gesture never adds a command; the status line shows `EXTRA TAP IGNORED`. After the second PREV finishes, whether it succeeded or failed, the gesture ends only after **800 ms with no further tap**. PREV stays clickable and NEXT stays disabled until then, and the second PREV's result stays visible. A continuing burst therefore never starts a third command. Without a second tap, a tap more than 800 ms after the first is a separate action. NEXT is never queued, and there is no automatic double-press, hold-to-repeat or SEEK. The CD timing row adds `prev queued · extra · cancelled` counts once a gesture has been used.

### CD track spacing (experimental)

`--cd-track-interval-ms` sets the spacing from one **completed** CD track dispatch to the next accepted one. It requires `--enable-cd-track`, accepts **200–1000** and defaults to **250**. The Gateway reports it as `min_interval_ms`, and the browser uses the same value. The Gateway prints it in the startup mode, for example `CD TRACK 250 ms`. Use `1000` to restore the Phase A spacing.

The value is an experimental setting for attended testing. It is not an established safe minimum.

- **Service:** separate commands cannot overlap. `CD_TRACK_*` runs synchronously and replies only after RELEASE. A concurrent command gets `CD_TRACK_BUSY`, and preflight refuses any fault or owed RELEASE. The service imposes no spacing after RELEASE, so the remaining question concerns the changer, not software state.
- **Evidence:** one research chain recorded as trial `id=34`: `EC → 72 → EC → 72` at a requested 100 ms gap (about 104–109 ms measured across that session, not per trial). The second, separate press moved to the preceding track. This is operator testimony from a single trial, without archived captures. It does not show that shorter gaps fail or that 100 ms is always enough.
- **Margins:** the 100 ms PRESS → RELEASE gap inside one pair says nothing about spacing between separate pairs. The default keeps 2.5× the only observed working RELEASE → PRESS gap, and the lower bound keeps 2×. On the Web path, the real spacing also includes the browser's token refresh, the Gateway's fresh STATUS read and service preflight, so it is always longer than the setting.
- **Unknown:** how long after a restart the changer still treats PREVIOUS as "move back". If two taps at 250 ms behave like two restarts, the cause is that changer threshold or slow UI round trips, not spacing that is too short. Record it and do not lower the bound.

### Rollout and rollback

Not performed by this change. Follow the [source-selection rollout pattern](source-selection.md): review `systemctl cat kenwood-xs-gateway.service`, add **only** `--enable-cd-track` (and, for the attended spacing trial, optionally an explicit `--cd-track-interval-ms`) to the existing Gateway drop-in while preserving `--systemd-socket`, `--nginx-uid`, `--https-origin` and any existing gates, run `systemd-analyze verify`, and stop only the Gateway so the next authenticated request activates it. The deployed XS8 Service must be a build that implements `CD_TRACK_*` and the CD STATUS fields. Attend the first trials with the changer visible and physical recovery available. For rollback, remove the flag and confirm `/api/v1/cd/track` returns 501 and PREV/NEXT are disabled.

Offline tests: `python3 -m unittest tests.test_gateway` (class `CdTrackGatewayTest`), `node tests/test_web_cd.js`, `node tests/test_web_cd_sequence.js` (consecutive fresh-token actions and duplicate browser events) `node tests/test_web_cd_rapid.js` (completion-based spacing, overlap and interval fallback), `node tests/test_web_cd_timing.js` and `node tests/test_web_cd_prev_gesture.js` (double-PREV gesture, cooldown, cancellation and no retry). They use fake services and establish Gateway routing, parsing and refusal behavior only.

## Future control API design (not implemented)

The following paths return HTTP 501 and do not access the XS8 service:

| Endpoint | Proposed request | Safety condition before implementation |
| --- | --- | --- |
| `POST /api/v1/volume/step` | `{ "direction": "UP" }` or `DOWN` | Implement a bounded, atomic step in the single-owner XS8 service. STOP obligation and failure handling must remain server-side; browser release events cannot be the sole stop mechanism. |
| `POST /api/v1/cd/transport` | None; superseded by `/api/v1/cd/track` | PLAY/PAUSE and STOP have no service API and no validated Web profile; ROOT `0x9C` is a STOP/CANCEL, not a KEY UP. Held SEEK (Phase B) needs a separately reviewed server-side token and heartbeat design. |

No new XS8 byte values are assigned by this API design. Future iOS clients can use the same versioned JSON endpoints and show capabilities before presenting enabled controls.

## LAN access plan

The direct LAN mode above is a temporary read-only exception. Before adding active controls, use the authenticated HTTPS reverse proxy and socket-activated Gateway, restrict the listener to the intended LAN, and protect every future mutating endpoint against cross-site requests. Browser clients can use a secure same-origin session; a future SwiftUI app can store its credential in Keychain. Do not forward this API to the public internet. Gateway socket permissions and XS8 Service socket permissions are separate local access boundaries.
