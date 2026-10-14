# Managed CEC client on Raspberry Pi

The [`kenwood-cec-xs-client.service.example`](../deploy/systemd/kenwood-cec-xs-client.service.example) template starts the existing `cec-xs-client` as `serg`. It opens the CEC adapter and sends volume and MUTE requests through `/tmp/kenwood-xs8.sock`. It has no GPIO access; `xs8-service` remains the sole XS8 GPIO owner. The unit has no dependency on `xs8-service`, so the CEC client can start while the service is absent. It does not start the old `cec-xs-bridge`, the Gateway, or `xs8-service`.

## Hold duration and safety review

- libCEC calls the client with a key and duration. The client treats `duration == 0` as a press or repeat, and `duration > 0` as release. Each accepted same-direction repeat sends another `VOLUME_PRESS` request. The service updates its last-event time without sending another XS8 START frame.
- The client and service each have a 1200 ms inactivity timer. Repeats refresh inactivity, but **neither absolute maximum hold is extended by repeats**. One absolute **20 s MAX_HOLD** applies to CEC and Web volume holds alike: the service's `--max-hold-ms` (100–20000 ms) and the client's (100–20000 ms) both default to 20000, and no client has its own different limit. The actual deployed setting must still be inspected separately: an older launch configuration may pass a shorter explicit value (for example 500 or 2500 ms), which keeps stopping holds at that point. Later repeats cannot prolong a hold; after `MAX_HOLD` or `INACTIVITY` the service and the client require a release before rearming.
- The unit sets the client cap to 20000 ms, the same as the service default. With equal caps the service usually reaches 20 s a few milliseconds first (its hold starts before the client's reply arrives); the client's next repeat is then refused and it logs `uncertain=PRESS_REPLY`, blocks the direction and waits for a release, as it does when the service stops a hold for any other reason. Continuous holding relies on the TV's repeats arriving at least every 1200 ms for the whole 20 s; a gap ends the hold with `INACTIVITY`, which fails safe.
- On release, inactivity, absolute cap, client shutdown, or socket loss, cleanup attempts STOP (`0x09`) before ROOT (`0x07`). The service retries uncertain cleanup at most three times per frame and latches a fault on failure. A successful GPIO TX or observed matching waveform does not prove that the receiver executed STOP. A crashed service, blocked bus, failed wiring, or killed process can defeat software cleanup.

The client makes no XS8 socket connection or command at startup. It opens a socket only for an accepted physical CEC key event. After an uncertain press or socket disconnect, it closes the connection, rejects more presses from the held direction, and requires a fresh release followed by a fresh press. Events older than its inactivity window or queued before its last stop are rejected. A process restart starts with an empty event queue; a CEC repeat first delivered **after** restart could still look like a new press, so observe restart during a held button before relying on this behavior.

The service's socket is created with restrictive permissions. Verify that the existing service runs as `serg` or that `serg` already has authorized access; do not loosen the socket mode or add nginx to its access group. The Gateway may receive `ERR BUSY` while this client owns the service's single connection during a volume hold. The original `cec-xs-bridge` must be stopped before enabling this unit because it is another CEC listener and GPIO owner.

## Install and start

These commands are for a reviewed Raspberry Pi deployment; they are not run as part of this repository change.

1. Build and run offline tests on the Pi: `make .build/cec-xs-client` and `make test`. Check `id serg`, access to the installed CEC adapter, and the existing XS8 socket owner/mode with `stat -c '%U:%G %a' /tmp/kenwood-xs8.sock`. Verify the service reports `state=IDLE`, `monitor=HEALTHY`, `stop_owed=0`, and `context_may_be_open=0` before motor testing. A missing XS8 socket does not prevent unit startup.
2. Inspect the live `xs8-service` command line or launch configuration for its actual `--max-hold-ms` value. To use the 20 s MAX_HOLD, remove an explicit shorter value or set `--max-hold-ms 20000`; a build without this change rejects values above 3000. Validate long holds attended, starting with short ones. Stop the original bridge and any manually started `cec-xs-client`; retain their prior launch instructions for rollback.
3. From the repository root, replace `__REPO_DIR__` with the repository's absolute path and install the resulting unit:

   ```sh
   sed "s|__REPO_DIR__|$PWD|g" deploy/systemd/kenwood-cec-xs-client.service.example | sudo tee /etc/systemd/system/kenwood-cec-xs-client.service >/dev/null
   sudo chmod 0644 /etc/systemd/system/kenwood-cec-xs-client.service
   sudo systemd-analyze verify /etc/systemd/system/kenwood-cec-xs-client.service
   sudo systemctl daemon-reload
   sudo systemctl enable --now kenwood-cec-xs-client.service
   ```

   The unit runs as `serg`, orders startup after local filesystems and udev, and restarts failures after five seconds. Systemd limits rapid failures to six starts per minute. It neither requires nor starts the XS8 Service. If the adapter is missing at startup, the process exits and systemd retries within that limit; after the limit is reached, investigate the adapter and use `sudo systemctl reset-failed kenwood-cec-xs-client.service` before starting again.

## Check operation

```sh
systemctl status kenwood-cec-xs-client.service
journalctl -u kenwood-cec-xs-client.service -b -f
```

With no remote key press, startup should log readiness and **no XS8 command**. Confirm the Web Remote remains usable when no CEC hold is active. With the receiver attended and at a safe listening level, test a short volume-down press and release, then a longer press within the reviewed caps, volume-up, and MUTE. Confirm START, STOP, ROOT, and their waveform evidence in the service logs and the receiver's physical behavior. Check missing RELEASE by observing an ordinary controlled test that ends at the inactivity or maximum-hold cap; do not intentionally leave the motor running. Check service loss, restart, and adapter disconnect/reconnect only with no active motor movement, then require a new press after release. The client currently opens the CEC adapter once; a mid-run adapter loss may stop callbacks without making the process fail, so systemd restart alone does **not** establish automatic adapter reconnection. The service watchdog and hold cap still bound a recognized hold when the service remains healthy. Validate actual adapter recovery before treating it as supported.

Stop cleanly with `sudo systemctl stop kenwood-cec-xs-client.service`. SIGTERM asks the client to release an active hold before exiting; the service also attempts cleanup when the socket closes. `TimeoutStopSec=20s` bounds shutdown, but forced termination or a failed STOP still requires direct receiver inspection. Review `journalctl -u kenwood-cec-xs-client.service -b` and the XS8 Service logs for `CLIENT_DISCONNECT`, `CEC_RELEASE`, `INACTIVITY`, `MAX_HOLD`, `STOP`, `ROOT`, and `FAULT_LATCHED` as applicable.

## Disable and rollback

```sh
sudo systemctl disable --now kenwood-cec-xs-client.service
sudo systemctl reset-failed kenwood-cec-xs-client.service
```

Wait for client exit and inspect the receiver before restoring the prior CEC frontend. If removing the installed unit, delete only `/etc/systemd/system/kenwood-cec-xs-client.service` after disabling it and run `sudo systemctl daemon-reload`. Restore the original bridge only after confirming the new client is stopped, so there is still just one XS8 GPIO owner. No Gateway or XS8 Service configuration is changed by the template.
