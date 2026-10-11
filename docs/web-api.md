# Web Remote preview and HTTP API v0.1

The Web Remote is currently a **read-only preview**. It displays the XS8 service status and available capabilities. Every control button is disabled. The gateway sends only `STATUS` to the local Unix socket; it never sends XS8 control commands.

## Run locally

Prerequisite: Python 3 with the standard library. From the repository root:

```sh
python3 xs8_gateway.py
```

Open `http://127.0.0.1:8080/` on the Raspberry Pi. The page still loads when `xs8-service` is unavailable and shows the connection failure. Start `xs8-service` separately if live status is needed, and run the gateway as a local user permitted to open its socket. The service's default socket is `/tmp/kenwood-xs8.sock`. Use `--socket PATH` for a different local socket or `--port PORT` for a different loopback port. The gateway always binds to `127.0.0.1`; this phase does not expose it to the LAN.

Offline tests (no GPIO or receiver access):

```sh
python3 -m unittest discover -s tests -p 'test_gateway.py'
```

## Implemented API

| Endpoint | Result |
| --- | --- |
| `GET /api/v1/status` | JSON with `gateway`, `xs8_service`, and parsed service `status`. Returns HTTP 503 when the service is unavailable or its reply is invalid. |
| `GET /api/v1/capabilities` | JSON classifications (`supported`, `unverified`, `unsupported`) and explicit `enabled` flags. Only status is enabled. |

`status` preserves the service's `last_gpio_tx`, `last_waveform`, and `receiver_execution` fields. `TX_OK` and `MATCH` do not prove that the Kenwood executed a command. The gateway opens one short-lived Unix socket connection per status request. It does not modify XS8 state.

## Future control API design (not implemented)

The following paths currently return HTTP 501 and do not access the XS8 service:

| Endpoint | Proposed request | Safety condition before implementation |
| --- | --- | --- |
| `POST /api/v1/source` | `{ "source": "CD" }` (or another documented source) | Validate each source command and receiver state on hardware. |
| `POST /api/v1/mute` | `{}` | Authenticate the caller and report transmission evidence separately from receiver execution. |
| `POST /api/v1/volume/step` | `{ "direction": "UP" }` or `DOWN` | Implement a bounded, atomic step in the single-owner XS8 service. STOP obligation and failure handling must remain server-side; browser release events cannot be the sole stop mechanism. |
| `POST /api/v1/cd/transport` | `{ "action": "PLAY_PAUSE" }`, `STOP`, `PREVIOUS`, or `NEXT` | Validate commands and context before exposure. |

No new XS8 byte values are assigned by this API design. Future iOS clients can use the same versioned JSON endpoints and show capabilities before presenting enabled controls.

## LAN access plan

Keep the gateway on loopback until authentication is implemented. For LAN use, put it behind a small HTTPS reverse proxy with authenticated access, restrict the listener to the trusted LAN, and require protection against cross-site requests on every future mutating endpoint. Browser clients can use a secure same-origin session; a future SwiftUI app can store its credential in Keychain. Do not forward this API to the public internet. Unix socket filesystem permissions remain a separate local access boundary.
