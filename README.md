# Kenwood XS Remote

Kenwood XS Remote controls a **Kenwood KR-V6070** receiver and its connected CD changer through the receiver's proprietary **SYSTEM CONTROL (XS8)** bus. A Raspberry Pi B+ drives the bus directly through GPIO. It serves two front ends:

- **HDMI-CEC:** volume and MUTE from the Panasonic TV and Apple TV remotes;
- **Web Remote:** an iPhone-friendly page over authenticated HTTPS, for power, input selection, TAPE2 MONITOR, volume, MUTE, CD transport and live receiver readback.

**Current status, validation levels and the live Pi configuration:** see [docs/project-status.md](docs/project-status.md). It is the canonical status and handoff document.

## Architecture

```text
Panasonic TV / Apple TV remote            iPhone Safari
        │ HDMI-CEC                              │ HTTPS + Basic Auth
        ▼                                       ▼
  cec-xs-client (libCEC)                nginx (LAN only, TLS)
        │                                       │ /run/kenwood-gateway.sock
        │                                       ▼
        │                          xs8_gateway.py (Gateway + web/ UI)
        │                                       │
        └───────────► /tmp/kenwood-xs8.sock ◄───┘
                               │
                       xs8-service (sole GPIO owner)
                               │ GPIO4/22 TX, GPIO27/23 RX sense
                               ▼
                 XS8 SYSTEM CONTROL → KR-V6070 ↔ CD changer
```

- **XS8 Service** (`xs8_service.cpp` and related files): the only process that touches GPIO. It sends each frame after a BUSY-free check and checks its waveform with a bus monitor. It also:
  - owns the volume, MUTE, CONTEXT, CD hold and POWER transactions;
  - latches faults instead of retrying;
  - decodes receiver broadcasts and `QUERY_STATUS` replies into source and MONITOR observations.
- **CEC client** (`cec_xs_client.cpp`): turns CEC volume and MUTE keys into service requests. It opens no GPIO.
- **Gateway** (`xs8_gateway.py`): an HTTP API on a systemd-activated Unix socket behind nginx. Every control has a default-off `--enable-*` gate, and each request needs a session, CSRF token, exact Origin and a fresh eligible STATUS. Requests are serialized by locks and spacing. The Gateway also runs the post-command SYNC readback and the one-click POWER decision.
- **Web Remote** (`web/`): the browser UI served by the Gateway.
- **Legacy:** `main.cpp` (`cec-xs-bridge`, its own GPIO owner) and the Arduino Micro bridge (`xs8-bridge/`) were the original path. They are kept as fallbacks only and must never run alongside the service.

## Capabilities at a glance

| Status | Capabilities |
|---|---|
| **Implemented and physically validated** | CEC volume UP/DOWN and MUTE • Web Remote over HTTPS with authentication • POWER ON (`0x52`) / OFF (`0x62`) and the one-click POWER toggle (both directions) • source selection CD, TUNER, PHONO, TAPE1, VIDEO1, VIDEO2 with automatic `QUERY_STATUS SETTLE` readback • TAPE2 MONITOR toggle (`0x17 → 0x48 → 0x07`; readback `0xD7` ON, `0xE7` OFF) • read-only `QUERY_STATUS` (`0x13`) and source/MONITOR observation • CD NEXT/PREVIOUS, held SEEK, STOP, PLAY/PAUSE, TIME DISP., REPEAT, DISC SKIP (the last three implemented, not yet deployed) |
| **Implemented, Web-path validation not recorded** | Web volume step and hold, and Web MUTE. They use the same service transactions as the validated CEC path. |
| **Experimental / disabled by default** | Every Gateway control gate (off in the checked-in unit) • CD changer presence probe • research TX, chain and CONTEXT REPL • zero CD track spacing • provisional SETTLE timing |
| **Planned / future** | Exclusive bus lease or service-side atomic POWER • archived trial notes for the latest validations • volume-step calibration • presence in the Web UI • native iOS client |

Some of these validations are operator reports without an archived trial note; [project status](docs/project-status.md) says which. A `TX_OK` / `MATCH` reply means only that a frame was transmitted and matched on the bus. It never proves the receiver executed it, and every service reply says `receiver_execution=UNVERIFIED`.

## Build and test

Offline (no GPIO, no receiver):

```sh
make test                                   # C++ units, service socket and REPL tests (fake GPIO backend)
python3 -m unittest tests.test_gateway      # Gateway
for t in tests/test_web_*.js; do node "$t"; done   # Web UI
```

Pi binaries are cross-built for ARMv6 with Docker ([pi-build](docs/pi-build.md)), or built natively with `make`. Deployment uses the templates in `deploy/` (systemd units and the nginx site), described in [cec-client-systemd](docs/cec-client-systemd.md), [gateway-systemd](docs/gateway-systemd.md) and [https-lan](docs/https-lan.md). The templates enable **no** control. Each Web control must be opted into on the Pi with its gate ([Web API](docs/web-api.md)).

## Precautions

This is an experimental interface for one documented KR-V6070 setup, not a general Kenwood compatibility claim.

- Verify the wiring, the 5 V TX stages and the RX dividers before transmitting ([hardware](docs/hardware.md)).
- A lost volume STOP or CD RELEASE can leave the receiver or changer running. Software cannot guarantee delivery on a blocked bus.
- Many researched bytes lock the front panel or change meaning with context. Only commands with validated mappings are exposed.
- The one-click POWER toggle is serialized only within the Gateway. Direct service clients and the receiver's own controls can still race it.

## Documentation

- [Project status and handoff](docs/project-status.md): capability status, evidence and live configuration
- [Architecture](docs/architecture.md) · [Hardware](docs/hardware.md) · [XS8 protocol](docs/xs8-protocol.md)
- Web and API: [Web API](docs/web-api.md), [receiver controls (VIDEO, MONITOR, POWER)](docs/receiver-controls-api.md), [source selection](docs/source-selection.md), [receiver status and QUERY_STATUS](docs/receiver-status.md), [MUTE](docs/mute-control.md), [volume step](docs/volume-step-api.md), [CD transport](docs/cd-transport-api.md), [CD changer presence](docs/cd-changer-presence.md)
- Deployment: [HTTPS on the LAN](docs/https-lan.md), [Gateway units](docs/gateway-systemd.md), [CEC client](docs/cec-client.md), [CEC client unit](docs/cec-client-systemd.md), [Pi build](docs/pi-build.md)
- Research: [research REPL](docs/xs8-research-repl.md), [command matrices (historical)](docs/xs8-command-matrices.md), [research archive](research/) ([history](research/RESEARCH_HISTORY.md), [command registry](research/COMMAND_REGISTRY.md), [coverage](research/XS8_COMMAND_COVERAGE.md))
