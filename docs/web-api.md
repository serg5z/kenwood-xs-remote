# Web Remote and HTTP API v0.1

The Gateway remains **read-only by default**. Explicit production `--enable-mute` and `--enable-source` options independently gate MUTE and the four evidenced source commands through authenticated HTTPS and the systemd Unix socket. TV1, TV2 and TAPE2 remain blocked pending controlled TX validation; volume and CD transport remain disabled. The checked-in systemd unit enables neither control. See [MUTE deployment](mute-control.md) and [source selection evidence and rollout](source-selection.md) before opting in.

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
| `GET /api/v1/session` | Returns 404 when both control gates are disabled; otherwise issues a secure session cookie and one-use CSRF token. |
| `POST /api/v1/mute` | Returns 501 with MUTE disabled. With the production gate, a valid request checks fresh XS8 eligibility and sends one `MUTE` to the local service. |
| `POST /api/v1/source` | Returns 501 with source disabled. With the production gate, accepts one allowlisted `{ "source": "CD" }` style JSON body, checks fresh XS8 eligibility, and dispatches at most one command. TV1, TV2 and TAPE2 return 409 without dispatch. |

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

## Future control API design (not implemented)

The following paths return HTTP 501 and do not access the XS8 service:

| Endpoint | Proposed request | Safety condition before implementation |
| --- | --- | --- |
| `POST /api/v1/volume/step` | `{ "direction": "UP" }` or `DOWN` | Implement a bounded, atomic step in the single-owner XS8 service. STOP obligation and failure handling must remain server-side; browser release events cannot be the sole stop mechanism. |
| `POST /api/v1/cd/transport` | `{ "action": "PLAY_PAUSE" }`, `STOP`, `PREVIOUS`, or `NEXT` | Validate commands and context before exposure. |

No new XS8 byte values are assigned by this API design. Future iOS clients can use the same versioned JSON endpoints and show capabilities before presenting enabled controls.

## LAN access plan

The direct LAN mode above is a temporary read-only exception. Before adding active controls, use the authenticated HTTPS reverse proxy and socket-activated Gateway, restrict the listener to the intended LAN, and protect every future mutating endpoint against cross-site requests. Browser clients can use a secure same-origin session; a future SwiftUI app can store its credential in Keychain. Do not forward this API to the public internet. Gateway socket permissions and XS8 Service socket permissions are separate local access boundaries.
