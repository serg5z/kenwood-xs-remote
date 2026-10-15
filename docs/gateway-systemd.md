# Gateway socket activation (read-only template)

The authenticated nginx route uses a systemd-owned Unix socket at `/run/kenwood-gateway.sock`. nginx still provides HTTPS and Basic Auth. The Gateway accepts HTTP only from the verified nginx worker UID and sends only `STATUS` to the local XS8 Service. Every POST remains HTTP 501. The XS8 Service is neither started nor required by these units, and remains the sole GPIO owner.

**Status (2026-10-15):** the units and nginx site are installed on the Pi. The checked-in templates are still read-only, but the live Gateway adds every `--enable-*` control gate through a systemd drop-in, so its POST endpoints are active. Opt-in controls are documented in the [Web API](web-api.md); the live configuration is recorded in [project status](project-status.md#live-deployment-on-the-pi-read-only-inspection-2026-10-15). The read-only statements below describe the template.

Templates: [socket unit](../deploy/systemd/kenwood-xs-gateway.socket.example) and [service unit](../deploy/systemd/kenwood-xs-gateway.service.example). These are deployment instructions; neither unit has been installed by this repository.

## Prepare the Pi

Check the actual nginx worker account and group on the Pi; do not infer them from the master process, which may run as root. Verify that unrelated applications do not use the same worker UID. Replace the placeholders:

| Placeholder | Meaning |
| --- | --- |
| `__NGINX_WORKER_GROUP__` | Verified nginx worker group. This must not be the XS8 Service socket group. |
| `__NGINX_WORKER_UID__` | Numeric UID of the verified nginx worker, not its group ID. |
| `__GATEWAY_USER__` / `__GATEWAY_GROUP__` | Dedicated unprivileged Gateway account and primary group. |
| `__REPO_DIR__` | Absolute repository path on the Pi. |

The socket unit sets owner `root`, the nginx worker group, and mode `0660` explicitly. The Gateway service receives the listening descriptor from systemd and checks each accepted connection with Linux `SO_PEERCRED` before handling HTTP. It opens no TCP listener in this mode. The service has `Restart=on-failure`, and the socket unit can activate it on demand. The Gateway still needs access to the **separate** XS8 Service socket to report live status. That socket currently has owner-only permissions; arrange narrow Gateway access separately and never give the nginx worker direct XS8 Service access or run the Gateway as root solely for status. `PrivateTmp=false` preserves access to the service's default `/tmp/kenwood-xs8.sock`.

## Install and validate, after review

First save the installed nginx site and any prior Gateway unit so rollback is possible. Complete both unit templates and the [nginx template](../deploy/nginx/kenwood-xs-remote.conf.example). Install the completed units as `/etc/systemd/system/kenwood-xs-gateway.socket` and `.service`, mode `0644`. The socket is the unit to enable; the service template has no `[Install]` section. The commands below are for a Pi administrator, not steps performed by this repository:

```sh
sudo install -o root -g root -m 0644 PATH_TO_EDITED_SOCKET /etc/systemd/system/kenwood-xs-gateway.socket
sudo install -o root -g root -m 0644 PATH_TO_EDITED_SERVICE /etc/systemd/system/kenwood-xs-gateway.service
sudo systemd-analyze verify /etc/systemd/system/kenwood-xs-gateway.socket /etc/systemd/system/kenwood-xs-gateway.service
sudo nginx -t
sudo systemctl daemon-reload
sudo systemctl disable --now kenwood-xs-gateway.service
sudo systemctl enable --now kenwood-xs-gateway.socket
sudo systemctl reload nginx
systemctl status kenwood-xs-gateway.socket kenwood-xs-gateway.service
stat -c '%U %G %a %F' /run/kenwood-gateway.sock
ss -lxnp
ss -ltnp
journalctl -u kenwood-xs-gateway.service -b --no-pager
```

The socket must be `root:<verified nginx worker group>` with mode `660`; there must be no Gateway TCP listener on port 8080. Check the full nginx configuration for an authenticated HTTPS site and Unix upstream, without an alternate HTTP proxy route. Through the HTTPS site, unauthenticated requests should receive 401, authenticated status should return 200 or 503 when XS8 Service is unavailable, and POST should return 501. A local client with an unrelated UID must fail to connect to the Unix socket. A process using the nginx UID but an incorrect/absent Basic Auth credential is still rejected by nginx when using the public route; direct socket access by that UID is within the nginx worker trust boundary. Do not forward port 443 or restore insecure LAN preview for control.

The matching systemd socket remains available across a Gateway process restart; Gateway sessions are in memory and therefore expire on restart. An nginx restart must still enforce Basic Auth. `/run` is recreated at boot. The socket unit removes its pathname when stopped, and the service is bound to that unit so it stops as well. Stop the service before stopping the socket during planned maintenance. Do not manually unlink a socket while its unit or service may be live. If a stale pathname remains after an abnormal stop, verify no listener holds it before cleanup.

## Rollback

Keep every control gate off. Stop the Gateway service, then disable and stop its socket unit. Restore the saved read-only loopback service configuration and nginx upstream together, verify with `systemd-analyze verify` and `nginx -t`, then reload nginx. Confirm the restored Gateway binds only `127.0.0.1` and every POST still returns 501. The old SSH-tunnel workflow can then target that read-only loopback listener. Rolling back the Web Gateway does not change XS8 Service, GPIO, CEC or motor-control behavior.
