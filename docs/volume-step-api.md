# XS8 Service atomic volume step (phase 1)

`VOLUME_STEP UP\n` and `VOLUME_STEP DOWN\n` ask the single-owner XS8 Service for **one bounded volume movement**. This is phase 1 of [Web Volume](web-api.md#future-control-api-design-not-implemented): the Gateway endpoint and Web controls do not exist yet, and `POST /api/v1/volume/step` still returns 501. The CEC client is unchanged and keeps using `VOLUME_PRESS` / `VOLUME_RELEASE`.

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

Offline tests only (`make test`: `volume_control_test`, `xs8_service_core_test`, `tests/test_xs8_service_socket.py`). No attended hardware trial has been run. Before any Gateway or Web exposure, an attended trial must establish the change per step, that every step stops, and the per-frame failure rate over repeated steps.
