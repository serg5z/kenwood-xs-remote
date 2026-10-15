# Project status and handoff

This is the **canonical status document** for people and AI agents picking up the project. It records what is implemented, what has been physically validated, and what is actually running on the Raspberry Pi. When another document disagrees about status, this one and the dated evidence it links take precedence. Detailed behaviour lives in the linked documents.

Status as of **2026-10-15**, repository commit `58a6a63`.

## Summary

The project is far beyond "volume and MUTE only". A Raspberry Pi drives the Kenwood KR-V6070's SYSTEM CONTROL (XS8) bus directly through GPIO. One XS8 Service owns the bus. Two front ends use it over a local Unix socket:

- the **CEC client**, for volume and MUTE from the TV and Apple TV remotes;
- the **Web Remote**, over authenticated HTTPS, for power, source selection, TAPE2 MONITOR, volume, MUTE, CD transport and live receiver readback.

The original Arduino Micro USB bridge is **legacy** and no longer in the control path.

## Evidence rules

- **Physically validated** means an operator watched the receiver or changer do the thing, in an attended trial. Dated notes are linked where they exist.
- `TX_OK` and waveform `MATCH` only show that the frame went out on the bus and matched. They **never** prove the receiver executed a command. Every service reply says `receiver_execution=UNVERIFIED`.
- A `QUERY_STATUS` readback reports what the receiver says about its source and MONITOR. It is an observation, not proof that a particular command caused it.
- Validation applies to this one KR-V6070 and its connected changer, not to other Kenwood models.
- Some validations are operator reports with no raw trace or trial note in the repository; the tables say so.

## 1. Implemented and physically validated

| Capability | Path | Evidence |
|---|---|---|
| Volume UP/DOWN (held) and MUTE from the Panasonic TV and Apple TV remotes | Remote → HDMI-CEC → `cec-xs-client` → XS8 Service → `0x17 → 0xF9/0xE9 … 0x09 → 0x07`; MUTE `0x17 → 0xD8 → 0x07` | Operator-validated working setup. MUTE confirmed as a toggle in both directions on 2026-10-12 ([protocol](xs8-protocol.md#working-control-sequences)). See the runtime note below: the CEC client is **not running on the Pi right now**. |
| Web Remote over HTTPS with Basic Auth | iPhone Safari → nginx (TLS, Basic Auth) → `/run/kenwood-gateway.sock` → Gateway → XS8 Service socket | In daily use on the Pi ([HTTPS](https-lan.md), [Gateway units](gateway-systemd.md)). Each control also needs a session cookie, a one-use CSRF token, an exact Origin and its own `--enable-*` flag. |
| POWER ON / OFF (explicit) | ROOT `0x52` ON, ROOT `0x62` OFF | ON 2026-10-12 ([note](../research/notes/xs8-root-validation-2026-10-12.md)); OFF is an operator statement, 2026-10-14 ([registry](../research/COMMAND_REGISTRY.md#operator-reported-physical-validations-2026-10-14-to-2026-10-15)). Neither is assumed idempotent ([receiver controls](receiver-controls-api.md#power_on--power_off)). |
| One-click POWER toggle | Gateway observes with `QUERY_STATUS SETTLE`: a reply means ON, so it sends `0x62`; two clean silences ≥ 1 s apart mean OFF, so it sends `0x52`; anything else sends nothing | Operator reports it physically verified **in both directions** (2026-10-15, after `58a6a63`). No trial note in the repository. Not globally atomic ([one-click POWER](web-api.md#one-click-power)). |
| Source selection: CD, TUNER, PHONO, TAPE1 (ROOT `0x84/0x82/0x83/0x85`), VIDEO1, VIDEO2 (CONTEXT `0x17 → 0x78/0x88 → 0x07`) | Web → Gateway → `SELECT_SOURCE` → automatic `QUERY_STATUS SETTLE` readback | VIDEO1/VIDEO2 sequences 2026-10-12 ([note](../research/notes/xs8-context-video-mute-2026-10-12.md)). The Web selection with automatic SETTLE readback is operator-validated (2026-10-15 report). Leaving a freshly observed CD also sends `CD_STOP` ([source selection](source-selection.md)). |
| TAPE2 MONITOR toggle | CONTEXT `0x17 → 0x48 → 0x07`, then SYNC readback | Operator reports physical verification on 2026-10-15. The readback reports MONITOR ON as waveform `0xD7` and OFF as `0xE7`. No trial note in the repository yet. The code still labels it `PROVISIONAL` (see [planned work](#4-planned-or-future-work)). |
| Read-only `QUERY_STATUS` (ROOT `0x13`) and passive source/MONITOR observation | Service query (plain or SETTLE); passive broadcast tracking | Source and MONITOR replies for all six sources and both MONITOR states, 2026-10-11 ([note](../research/notes/xs8-readback-2026-10-11.md), [receiver status](receiver-status.md)). |
| CD changer NEXT / PREVIOUS (discrete `0xFC/0xEC → 0x72`), held SEEK (`0x7A/0x6A … 0x72`) | Web → Gateway → service | 2026-10-12 ([CD hold note](../research/notes/xs8-cd-hold-api-validation-2026-10-12.md), [KEY RELEASE note](../research/notes/xs8-cd-key-release-2026-10-12.md)). The service note covers held SEEK_FORWARD; the paired-frame research note covers both seek directions. |
| CD STOP (ROOT `0xA1`) | Web → Gateway → `CD_STOP` | Physically verified on the KR-V6070 (operator statement, recorded in commit `2793883`, 2026-10-14; [CD transport](cd-transport-api.md#single-frame-keys-play-playpause-and-stop)). The historical ROOT `0x9C` STOP is no longer sent. |
| CD PLAY/PAUSE (ROOT `0xBC`) | Web → Gateway → `CD_PLAY_PAUSE` | The operator reports earlier physical testing (stated 2026-10-15). No dated trial note is archived, and the API still returns `"mapping": "PROVISIONAL"`. |
| Safety model | Single GPIO owner; STOP-before-ROOT; watchdogs and a 20 s maximum hold; fault latching; no automatic retry of state-changing commands; Gateway eligibility checks and per-control locks and spacing | Exercised in the attended trials above. Covered by the offline tests (`make test`, `tests/test_gateway.py`, `tests/test_web_*.js`). |

## 2. Implemented, physical validation not recorded

These are built, enabled on the Pi and covered by offline tests. The repository has no dated physical trial for **this path**, even where the underlying XS8 sequence is validated.

| Capability | Notes |
|---|---|
| Web volume step (`VOLUME_STEP`, `0x17 → 0xF9/0xE9 → 0x09 → 0x07`) | Same frames as the validated CEC volume path. The size of one step is not calibrated ([volume step](volume-step-api.md)). |
| Web volume hold (connection-owned, 20 s cap) | Same service hold machinery as CEC ([Web API](web-api.md#web-remote-v2-controls)). |
| Web MUTE | Same service `MUTE` transaction as CEC ([MUTE deployment](mute-control.md)). The Web never shows a mute state: there is no readback. |
| Diagnostic power observation (`ON` / `OFF_INFERRED` / `UNKNOWN`) | In Gateway memory only; lost on restart. `OFF_INFERRED` is an inference from silence. |
| `CD_PLAY` (`0x41`) and held TRACK via `CD_PRESS` | Socket-only, not exposed by the Gateway. `0x41` was physically observed in research on 2026-10-12. |

## 3. Experimental or disabled by default

| Feature | Default | On the Pi (2026-10-15) |
|---|---|---|
| Every Gateway control gate (`--enable-mute`, `-source`, `-receiver-query`, `-volume`, `-volume-hold`, `-cd-track`, `-cd-playback`, `-cd-seek`, `-power`, `-tape2-monitor`) | Off. The checked-in Gateway unit is read-only | **All on** |
| `--cd-track-interval-ms 0` (no spacing between CD track taps) | Default spacing applies | **On** (experimental) |
| CD changer presence probe (`PROBE_CD_CHANGER`, ROOT `0xC3`, `--enable-cd-presence-probe`) | Off; socket-only; Gateway and Web never send it ([presence](cd-changer-presence.md)) | Off |
| Research TX, chain and CONTEXT session (`--enable-research-tx`, `--enable-research-chain`, `--enable-research-context`; [REPL](xs8-research-repl.md)) | Off | **On** in the service (attended research use) |
| `QUERY_STATUS SETTLE` windows (600 ms TX quiet, 200 ms bus quiet, 1.5 s max) and the 2 × silence rule for OFF | n/a | Provisional values, in use |
| Legacy `cec-xs-bridge` (`main.cpp`, its own GPIO owner) and the Arduino bridge (`xs8-bridge/`) | Fallback only; never run alongside the service | Legacy unit installed and enabled, but not running |

## 4. Planned or future work

- An exclusive bus lease or service-side atomic POWER toggle. Deliberately deferred: the toggle is Gateway policy and **not globally atomic**.
- Dated trial notes for the operator-reported validations: POWER OFF, the POWER toggle, TAPE2 MONITOR, Web source selection with readback, `0xA1` STOP and CD PLAY/PAUSE. They are summarised with their evidence level in the [command registry](../research/COMMAND_REGISTRY.md#operator-reported-physical-validations-2026-10-14-to-2026-10-15) and [coverage map](../research/XS8_COMMAND_COVERAGE.md#4-physical-validations-reported-2026-10-12-to-2026-10-15). Once trial notes exist, the `PROVISIONAL` labels in code can be lifted.
- Calibrating the volume step; any mute-state readback (none is known).
- Showing CD changer presence in the Web or using it in the Gateway (diagnostic only today).
- Tuning the SETTLE timing from attended evidence.
- Validating CEC adapter reconnection, and archiving end-to-end raw traces.
- A possible native iOS client on the same versioned API.

Commands with no validated mapping are not exposed: TAPE2 as a source, CONTEXT `0x39` power-off, the ROOT front-panel lock family, and tuner presets and scanning.

## Live deployment on the Pi (read-only inspection, 2026-10-15)

Inspected with `systemctl` and `ps` on `woodland-pi.local`. Nothing was changed. Treat this as a snapshot and re-check it before relying on it.

| Component | Unit | State | Command line |
|---|---|---|---|
| XS8 Service | `kenwood-xs8.service` (drop-in) | **active**, but **disabled at boot** | `/tmp/xs8-service-v7 --allow-unrecovered-start --max-hold-ms 20000 --enable-research-tx --enable-research-chain --enable-research-context`. The binary lives in `/tmp` (built 2026-10-15 07:48), so a reboot removes it. Its exact source commit is not recorded. |
| Gateway | `kenwood-xs-gateway.socket` + `.service` (drop-in) | active; socket enabled | `--systemd-socket --nginx-uid 33 --https-origin https://woodland-pi.local` plus every `--enable-*` gate listed above and `--cd-track-interval-ms 0`. The Pi checkout is at `58a6a63`. The unit description still says "read-only". |
| nginx | `nginx.service` | active | HTTPS on the LAN address, port 443, Basic Auth, `proxy_read_timeout 14s` |
| CEC client | `kenwood-cec-xs-client.service` (drop-in `--max-hold-ms 2000`) | enabled, but **inactive since 2026-10-12 22:45** (clean exit) | Not running at inspection time, so CEC volume/MUTE was not available then |
| Legacy bridge | `kenwood-xs-remote.service` (`cec-xs-bridge`) | enabled, inactive | Must not run alongside the XS8 Service: it is a second GPIO owner |

The deployment risks this shows are for the operator to decide on; they are not fixed in the repository:

- After a reboot the XS8 Service would not start (its unit is disabled), and its `/tmp` binary would be gone.
- Both CEC front ends are enabled at boot, but the XS8 Service is not. After a reboot, the legacy `cec-xs-bridge` would start as a GPIO owner, and `cec-xs-client` would start with no service to talk to. Both would listen to the same CEC keys; normally only one front end should be enabled.
- Research TX, chain and CONTEXT are enabled on the live service, so any local socket client can use them.
- The CEC client's live cap is 2 s, while the template uses 20 s.

## Where to look next

[README](../README.md) · [Architecture](architecture.md) · [Web API](web-api.md) · [Receiver controls](receiver-controls-api.md) · [Source selection](source-selection.md) · [Receiver status](receiver-status.md) · [CD transport](cd-transport-api.md) · [Hardware](hardware.md) · [Protocol](xs8-protocol.md) · [Research history](../research/RESEARCH_HISTORY.md)
