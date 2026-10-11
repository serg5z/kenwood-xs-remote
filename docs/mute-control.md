# Production Web MUTE: opt-in deployment and rollback

The checked-in Gateway systemd unit is read-only. `--enable-mute` is a separate, default-off production gate. It works only with systemd Unix-socket activation, a verified nginx worker UID, an exact HTTPS Origin, and the default `/tmp/kenwood-xs8.sock` service socket. The fake-only `--enable-offline-mute` mode remains separate. Neither mode enables volume, source selection, or CD transport.

`OK MUTE_TX` reports that the XS8 Service completed its MUTE transmission path. It does not establish that the receiver physically muted. A timeout, disconnect, or `ERR MUTE_UNCERTAIN` is not retried. The service rejects MUTE in `BOOT_INHIBIT` and `FAULT_LATCHED`; the Gateway also requires a fresh `IDLE`, `HEALTHY`, no-stop-owed, closed-context STATUS immediately before dispatch. The final authorization still belongs to the XS8 Service.

## Before enabling on Raspberry Pi

1. Run the offline Gateway suite from the repository root: `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_gateway.py'`. If Node.js is installed, also run `node tests/test_web_mute.js`. These tests use temporary sockets and fake service responses, never the physical receiver.
2. Finish the isolated live nginx identity probe: a request authenticated as `kenwood` with a forged `X-Kenwood-Authenticated-User: intruder` must deliver only `kenwood` to the probe upstream. Missing or incorrect Basic Auth must never reach it. Remove the temporary nginx location and socket afterward. This verification has **not** been performed by the repository tests.
3. Review `sudo nginx -T`: the active site must use validated HTTPS for `woodland-pi.local`, Basic Auth for the whole UI/API, `proxy_set_header X-Kenwood-Authenticated-User $remote_user`, `proxy_set_header X-Forwarded-Proto https`, and the Unix upstream `/run/kenwood-gateway.sock`. Check for alternate unauthenticated routes. Run `sudo nginx -t`.
4. Confirm `stat -c '%U %G %a %F' /run/kenwood-gateway.sock` shows `root:www-data 660 socket`, the nginx worker UID is 33, and `ss -ltnp` shows no Gateway TCP listener. Confirm the dedicated Gateway account can open `/tmp/kenwood-xs8.sock`; do not give `www-data` direct access to that service socket.
5. Check authenticated `/api/v1/status`. Continue only if `state=IDLE`, `monitor=HEALTHY`, `stop_owed=0`, and `context_may_be_open=0`. If the service reports `BOOT_INHIBIT`, stop. Its operator authorization is configured in the XS8 Service, not granted by the Gateway.

## Enable MUTE explicitly

Inspect `systemctl cat kenwood-xs-gateway.service` and use its **actual absolute repository path** below. Do not change nginx or the Gateway socket unit. Create a dedicated systemd drop-in, replacing `__REPO_DIR__`:

```sh
sudo install -d -o root -g root -m 0755 /etc/systemd/system/kenwood-xs-gateway.service.d
sudo tee /etc/systemd/system/kenwood-xs-gateway.service.d/30-mute.conf >/dev/null <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/bin/python3 __REPO_DIR__/xs8_gateway.py --systemd-socket --nginx-uid 33 --enable-mute --https-origin https://woodland-pi.local
EOF
sudo systemctl daemon-reload
sudo systemd-analyze verify kenwood-xs-gateway.service kenwood-xs-gateway.socket
sudo systemctl stop kenwood-xs-gateway.service
```

An authenticated HTTPS GET to `/api/v1/status` activates the new Gateway through the existing socket. Check `systemctl status kenwood-xs-gateway.service`, `journalctl -u kenwood-xs-gateway.service -b --no-pager`, and `systemctl cat kenwood-xs-gateway.service`; the Gateway log must say `PRODUCTION MUTE`. Recheck `/api/v1/status` and `/api/v1/capabilities` through HTTPS with certificate validation. MUTE should be available only with an authenticated session and eligible service state. Loading the page or obtaining a session sends no MUTE.

## One controlled physical test

With the receiver attended, no volume motor moving, and the source level low enough that an unexpected unmute is safe, open the HTTPS Web Remote on the iPhone. Check the diagnostic state is eligible and the MUTE button is enabled. Press **MUTE once**. Observe the receiver directly and compare it with the UI's transmission result. Do not infer receiver action from `SERVICE_TX_OK`; do not press again to compensate for a timeout or uncertain result. If the result is uncertain, the receiver does not respond, or any other behavior is unexpected, use the front panel as needed and roll the Gateway back to read-only before investigating.

## Roll back to read-only

Remove only the dedicated `30-mute.conf` drop-in created above, leaving any other overrides intact:

```sh
sudo systemctl stop kenwood-xs-gateway.service
sudo rm /etc/systemd/system/kenwood-xs-gateway.service.d/30-mute.conf
sudo systemctl daemon-reload
sudo systemd-analyze verify kenwood-xs-gateway.service kenwood-xs-gateway.socket
```

An authenticated HTTPS GET activates the default read-only Gateway. Confirm its log says `read-only`, `/api/v1/session` returns 404, `/api/v1/mute` returns 501 without contacting the XS8 Service, and the Web MUTE button is disabled after reload. The existing nginx and XS8 Service configurations are unchanged by this rollback.
