# Read-only gateway systemd unit

This unit starts only the Python Web Gateway after boot. It binds to `127.0.0.1:8080` and sends only `STATUS` to the local XS8 socket. It does **not** start or require `xs8-service`, and it does not enable any control route: every POST remains HTTP 501. Keep nginx responsible for LAN HTTPS and Basic Auth; see the [HTTPS setup guide](https-lan.md).

The template is [deploy/systemd/kenwood-xs-gateway.service.example](../deploy/systemd/kenwood-xs-gateway.service.example). These are **deployment instructions**; this repository has not installed or enabled the unit.

## Prepare the Pi

Confirm the repository path and Python location on the Pi (`pwd -P` from the repository root and `command -v python3`). The template assumes `/usr/bin/python3`; adjust it if needed. Replace all placeholders before installing:

| Placeholder | Meaning |
| --- | --- |
| `__GATEWAY_USER__` | Existing unprivileged local account for the gateway. Never `root`. |
| `__GATEWAY_GROUP__` | That account's primary group. |
| `__REPO_DIR__` | Absolute path to the repository on the Pi. |

The unit uses `After=network.target`. This orders it after network initialization without waiting for a LAN address, because it binds only to loopback. `Restart=on-failure` restarts unexpected exits. It has no `Requires=` or `After=` relationship with `xs8-service`; the gateway can serve the UI and report HTTP 503 for STATUS while the XS8 service is absent. [systemd network target guidance](https://wiki.freedesktop.org/www/Software/systemd/NetworkTarget/).

The service creates `/tmp/kenwood-xs8.sock` with owner-only permissions (`umask 0077`). If `xs8-service` runs under another account, especially root, this unprivileged gateway **cannot** read its socket and STATUS will report unavailable even when the service runs. Choose a compatible local ownership arrangement before relying on live status; do not make the socket world-accessible. This unit does not change the XS8 service or its socket permissions. `PrivateTmp=false` is intentional so the gateway can see the same `/tmp` socket.

## Install and validate on the Pi

Edit a copy of the template with the actual values, then install that completed copy as `/etc/systemd/system/kenwood-xs-gateway.service` (mode `0644`). Do not install the file with unresolved `__PLACEHOLDERS__`. The example commands below are for the Pi administrator after review; replace `PATH_TO_EDITED_COPY` with the completed file's path:

```sh
sudo install -o root -g root -m 0644 PATH_TO_EDITED_COPY /etc/systemd/system/kenwood-xs-gateway.service
sudo systemd-analyze verify /etc/systemd/system/kenwood-xs-gateway.service
sudo systemctl daemon-reload
sudo systemctl enable --now kenwood-xs-gateway.service
systemctl status kenwood-xs-gateway.service
journalctl -u kenwood-xs-gateway.service -b --no-pager
ss -ltnp
curl -i http://127.0.0.1:8080/api/v1/capabilities
curl -i http://127.0.0.1:8080/api/v1/status
curl -i -X POST http://127.0.0.1:8080/api/v1/mute
```

Expect capabilities HTTP 200, STATUS HTTP 200 when an accessible XS8 service responds or HTTP 503 when it does not, and POST HTTP 501. `ss` must show only `127.0.0.1:8080` for the gateway, never a LAN address or `0.0.0.0:8080`. Ensure the temporary `--allow-insecure-readonly-lan` process is stopped before enabling the unit. Verify nginx separately over HTTPS and Basic Auth; do not forward port 8080 or 443 through the router.

For live logs, use `journalctl -u kenwood-xs-gateway.service -f`. To check reboot behavior, inspect `systemctl status` and the current boot's journal after a planned reboot. The gateway reports XS8 absence in its API response and stays running; nginx can continue serving the authenticated UI.

## Disable and roll back

```sh
sudo systemctl disable --now kenwood-xs-gateway.service
sudo systemctl daemon-reload
```

After disabling the unit, the gateway can be run manually on loopback with `python3 xs8_gateway.py`. The existing Mac SSH tunnel remains available (`ssh -N -L 8080:127.0.0.1:8080 USER@woodland-pi.local`), then open `http://127.0.0.1:8080/` on the Mac. If rolling back nginx too, disable only its Kenwood site as described in the [HTTPS guide](https-lan.md); do not enable unauthenticated LAN HTTP for routine use. Disabling this unit does not affect `xs8-service`, GPIO, CEC, or motor control.
