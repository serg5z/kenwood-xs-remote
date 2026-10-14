# XS8 Service atomic volume step (phase 1)

`VOLUME_STEP UP\n` and `VOLUME_STEP DOWN\n` ask the single-owner XS8 Service for **one bounded volume movement**. The Gateway exposes it as [`POST /api/v1/volume/step`](web-api.md#volume-step-phase-1) behind the default-off `--enable-volume`, for single taps only. The CEC client is unchanged and keeps using `VOLUME_PRESS` / `VOLUME_RELEASE`.

## Sequence

One request sends four separate logical TX frames, in this order, each with the usual BUSY-free check and waveform verification:

| Direction | Frames |
|---|---|
| `UP` | `0x17 → 0xF9 → 0x09 → 0x07` |
| `DOWN` | `0x17 → 0xE9 → 0x09 → 0x07` |

There is **no added hold delay**: STOP is sent as soon as the start frame is verified, so the transmission time of the STOP frame (roughly 100 ms by the timing constants; not measured) is the minimum movement. The amount of volume change per step has **not been physically calibrated**. The step reuses `VolumeControl::start()` and `finish()`. The STOP and CONTEXT obligations are recorded before each frame may reach the bus, STOP and ROOT exit get the existing three attempts each, and any unverified frame latches `FAULT_LATCHED`. The step as a whole is **never retried**.

## Replies

- `OK VOLUME_STEP_TX direction=UP state=IDLE stop_owed=0 context_may_be_open=0 receiver_execution=UNVERIFIED`: all four frames completed with `TX_OK`/`MATCH`. This does not prove the receiver changed volume.
- `ERR VOLUME_STEP_UNCERTAIN direction=… state=FAULT_LATCHED stop_owed=0|1 context_may_be_open=0|1 receiver_execution=UNVERIFIED`: a frame or its cleanup was unverified. The fault blocks every further XS8 transmission (volume, MUTE, CD, source, query) until an attended service restart. `stop_owed=1` or `context_may_be_open=1` means the receiver may still be changing volume or be in CONTEXT: check it physically. Graceful shutdown makes one more ordered cleanup attempt.
- `ERR VOLUME_STEP_CANCELLED …`: shutdown was signalled after the entry or start frame. STOP and exit still ran; the movement may or may not have happened.
- Refusals transmit nothing:
  - `ERR VOLUME_BUSY` while any volume hold (CEC) is active; the hold is not interrupted and its owner is unchanged;
  - `ERR CD_TRACK_BUSY` or `ERR CD_FAULT_LATCHED`;
  - `ERR RESEARCH_CONTEXT_ACTIVE`;
  - `ERR MONITOR_FAILED`;
  - `ERR BOOT_INHIBIT`, `ERR FAULT_LATCHED` or another non-IDLE volume state;
  - `ERR UNRESOLVED_STATE`;
  - `ERR SHUTDOWN`, or `ERR RESEARCH reason=TX_UNCERTAIN_LATCHED` / `SESSION_BUSY`, as for CD commands;
  - `ERR NO_CLIENT`.

The step does not set or clear the CEC re-arm block: after a CEC hold is stopped by its watchdog, a step is allowed, and the same-direction CEC press still needs its release first. A step creates no ownership, so closing the requesting connection owes nothing. The journal records `XS8 VOLUME_STEP direction=… result=TX_VERIFIED|CANCELLED|UNCERTAIN … receiver_execution=UNVERIFIED` and the existing `XS8 STOP reason=STEP` line.

## Validation status

Offline tests: `make test` (`volume_control_test`, `xs8_service_core_test`, `tests/test_xs8_service_socket.py`). The operator reported that initial attended tests of both directions on the KR-V6070 passed (2026-10-13); the trial details, the change per step and a per-frame failure rate over repeated steps are not recorded here. Hold-to-repeat should wait for that failure-rate evidence.

## Connection-owned volume hold

`KEY_DOWN VOLUME_UP|VOLUME_DOWN` and `KEY_UP VOLUME_UP|VOLUME_DOWN` give the Web a held volume control on the **same VolumeControl path as CEC's `VOLUME_PRESS`**: `press()`, `release()` and the owner-disconnect `disconnect()`. Ownership is the socket connection that sent `KEY_DOWN`; no new framework or timer was added.

| Request (owning connection unless noted) | Reply | XS8 TX |
|---|---|---|
| `KEY_DOWN A` while volume and CD are idle | `OK KEY_DOWN_TX action=A gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED` | `0x17 → 0xF9` (UP) or `0xE9` (DOWN) |
| `KEY_DOWN A` again, same direction | `OK KEY_ALIVE action=A` (renews the 1200 ms inactivity) | none |
| `KEY_DOWN` during any other hold (other direction, other connection, CEC, CD) | `ERR KEY_BUSY` | none |
| `KEY_UP A` | `OK KEY_UP_TX action=A …` or `ERR KEY_UP_UNCERTAIN action=A state=… stop_owed=… context_may_be_open=… receiver_execution=UNVERIFIED` | `0x09 → 0x07` |
| `KEY_UP` naming the other direction | `ERR KEY_ACTION_MISMATCH` | none |
| `KEY_UP` from another connection during a hold | `ERR KEY_NOT_OWNER` | none |
| `KEY_UP` with no hold | `OK KEY_UP_NONE last=<state>` | none |

An uncertain start replies `ERR KEY_DOWN_UNCERTAIN action=A state=FAULT_LATCHED …`; `finish()` has already attempted `0x09 → 0x07`. The hold ends by `KEY_UP`; by its **owner connection closing** (`CLIENT_DISCONNECT`: `0x09 → 0x07` through the normal event loop); by **1200 ms without `KEY_DOWN`** (`INACTIVITY`); by the service's `--max-hold-ms` (`MAX_HOLD`); or by shutdown. Every STOP and exit failure latches `FAULT_LATCHED` exactly as for CEC.

**Re-arm.** After `INACTIVITY` or `MAX_HOLD`, VolumeControl's existing re-arm block refuses the owner's late keepalive (`ERR KEY_EXPIRED last=VOLUME_WATCHDOG`), so it can never start a new press. The owner's `KEY_UP` clears it, as CEC's `VOLUME_RELEASE` does. Because a closed connection can send no stale keepalive, the disconnect of a **`KEY_DOWN` owner** also clears its block (no frame), so an abandoned Web hold never leaves CEC VOL+/− blocked. CEC (`VOLUME_PRESS`) owners keep the existing behavior: their disconnect does not clear the block.

**Owner checks (all clients).** During any volume hold, `VOLUME_RELEASE` from a non-owner now answers `ERR VOLUME_NOT_OWNER` and `MUTE` from a non-owner answers `ERR VOLUME_BUSY`, both without transmitting: no client can end another's hold. The CEC client only releases and mutes on its own owning connection, so its behavior is unchanged; `VOLUME_RELEASE` while idle still answers OK (clearing a matching re-arm block), and the research gates still admit it.

Single-tap `VOLUME_STEP` is unchanged and refused (`ERR VOLUME_BUSY`) during any hold.
