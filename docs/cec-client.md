# Standalone CEC client for XS8 Service

`cec-xs-client` is an optional, manually started CEC frontend for the existing single-owner `xs8-service`:

```text
Apple TV / onn remote → HDMI-CEC → libCEC → cec-xs-client
    → /tmp/kenwood-xs8.sock → xs8-service → XS8 GPIO → KR-V6070
```

The client opens no GPIO lines and sends no XS8 frames. It maps libCEC volume-up/down keypresses to `VOLUME_PRESS UP|DOWN`, key releases (`duration > 0`) to `VOLUME_RELEASE UP|DOWN`, and a MUTE press to one `MUTE`. Repeated volume presses refresh the service's hold watchdog without transmitting another START frame. The service remains responsible for STOP-before-ROOT, monitor verification, fault latching and its independent `MAX_HOLD`. The original [`cec-xs-bridge`](../main.cpp) remains an unchanged fallback.

The callback timestamps each event with `steady_clock`; the client uses that timestamp to reject old presses and log processing age. The current socket protocol does **not** carry a timestamp, so the service sees its own command receipt time. The callback queue has a fixed 32-event capacity. Overflow ends any active client hold and discards the uncertain queue. A local 1200 ms inactivity timer and configurable maximum hold request release; the service's own limits remain independent. A CEC release requests `VOLUME_RELEASE` and closes the connection. Socket failure or uncertain replies close the connection so the service can attempt disconnect cleanup. The client does not reconnect and resume an old hold: it waits for a fresh release followed by a fresh press. No command is sent on startup or simply because the service reappears.

The service handles only one socket client at a time. This client holds its connection only during a volume hold; MUTE uses a short connection. Web Gateway status or control requests may receive `ERR BUSY` while CEC volume is held. Concurrent use of the original bridge and this client is **not** supported: both would independently interpret the same CEC keys, and the bridge would still own GPIO.

## Offline build and checks on Raspberry Pi

Prerequisites: a C++17 compiler, libCEC headers and library, and POSIX Unix sockets. Build the new client separately from the GPIO targets:

```sh
make .build/cec-xs-client
make test
```

The new `cec_client_core_test` exercises press/release, repeated and out-of-order events, missing release, stale press rejection, disconnect, reconnection without replay, MUTE deduplication, and queue overflow. `cec_service_socket_test` uses a temporary fake Unix socket server. The tests send no XS8 commands and require no receiver. On a restricted development host, the fake Unix socket test may need permission to bind a temporary socket; no hardware access is involved.

## Manual Pi deployment

1. Build and pass the offline tests on the Pi. Verify the existing `xs8-service` is already running under its intended operator authorization and reports `state=IDLE`, `monitor=HEALTHY`, `stop_owed=0`, and `context_may_be_open=0`. The client does not bypass BOOT_INHIBIT or faults. Ensure the account running the client can access the service socket; the service creates it with restrictive permissions. Do not broaden socket permissions for unrelated users.
2. Stop the old `cec-xs-bridge` process before starting the new client. Keep its binary and launch configuration for rollback. Do not run both CEC frontends or both GPIO owners at once.
3. With the receiver attended, start manually from the repository root:

   ```sh
   ./.build/cec-xs-client --socket /tmp/kenwood-xs8.sock --max-hold-ms 2000
   ```

   Match the maximum hold to the reviewed service configuration; the service's own limit remains authoritative. No socket or XS8 command is opened at startup. Test one short volume-down press/release first, then up and MUTE separately while watching the receiver. Inspect the client and service logs for `VOLUME_RELEASE`, STOP waveform evidence, and faults. A successful socket reply or TX waveform does not prove receiver execution. Stop validation immediately if the motor continues unexpectedly.

There is no new systemd unit or autostart in this phase. The executable does not restart `xs8-service` or configure CEC adapters. If the service is unavailable, a press fails closed until its release; recovery requires a new physical press after the service is healthy. If the service process crashes while the motor is active, software cannot guarantee STOP delivery. Physical receiver observation and a manual stop path remain necessary.

## Rollback

Send SIGINT or SIGTERM to `cec-xs-client` and wait for it to exit; it attempts `VOLUME_RELEASE` for an active hold, then closes the socket. Check the receiver directly before any restart. Restore the previously working `cec-xs-bridge` using its prior manual launch procedure only after the new client has exited. The XS8 Service, GPIO timing, Web Gateway, and existing bridge source remain unchanged by this rollout.
