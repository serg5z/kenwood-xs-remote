# Kenwood XS Remote — Raspberry Pi Recovery Runbook

**Host:** `woodland-pi`  
**System:** Kenwood KR-V6070 + DP-R3070, Raspberry Pi B+  
**Purpose:** Restore remote-control functionality when Web, iPhone or CEC controls stop responding.

## 1. Quick recovery

Connect to the Raspberry Pi:

```bash
ssh woodland-pi
```

Check services:

```bash
systemctl --failed

systemctl --no-pager status \
  kenwood-xs8 \
  kenwood-xs-gateway \
  kenwood-cec-xs-client
```

If there is no active research session or held VOL/SEEK command, restart the control chain in dependency order:

```bash
sudo systemctl restart kenwood-xs8.service
sudo systemctl restart kenwood-xs-gateway.service
sudo systemctl restart kenwood-cec-xs-client.service
```

Check again:

```bash
systemctl --no-pager --full status \
  kenwood-xs8.service \
  kenwood-xs-gateway.service \
  kenwood-cec-xs-client.service
```

Test Web/iPhone Remote and volume control through CEC.

**Known successful recovery:** October 25, 2026. All three services were reported active/running, but remote control was not working. Restarting the chain restored functionality.

## 2. Services

| Service | Responsibility |
|---|---|
| `kenwood-xs8.service` | XS8 protocol and GPIO access; sole owner of the XS8 bus interface |
| `kenwood-xs-gateway.service` | Web/iPhone Remote gateway |
| `kenwood-cec-xs-client.service` | HDMI-CEC commands forwarded to XS8 |
| `nginx` | HTTPS and reverse proxy for the Web Remote |

**Do not unmask:**

`kenwood-xs-remote.service`

This is an obsolete, intentionally masked service. Its inactive/dead state is expected.

## 3. Troubleshooting by symptom

### Web Remote and iPhone Remote fail

Check Gateway and XS8:

```bash
systemctl status kenwood-xs-gateway kenwood-xs8
```

Inspect recent errors:

```bash
journalctl -u kenwood-xs-gateway -n 100 --no-pager
journalctl -u kenwood-xs8 -n 100 --no-pager
```

If XS8 is healthy and only Web/iPhone control fails, restart the Gateway first:

```bash
sudo systemctl restart kenwood-xs-gateway
```

If the website itself is unreachable, also check:

```bash
systemctl status nginx
journalctl -u nginx -n 50 --no-pager
```

### Apple TV remote volume stops working

Check the CEC client:

```bash
systemctl status kenwood-cec-xs-client
journalctl -u kenwood-cec-xs-client -n 100 --no-pager
```

If Web/iPhone controls still work, try restarting only CEC:

```bash
sudo systemctl restart kenwood-cec-xs-client
```

### All remote-control paths fail

Suspect XS8 Service or its communication with clients.

Check:

```bash
systemctl status kenwood-xs8
journalctl -u kenwood-xs8 -n 150 --no-pager
```

If nothing obviously explains the failure, use the full three-service recovery sequence from section 1.

### Web page loads, but buttons do nothing

The HTTPS/web layer may be working while the Gateway or XS8 command path is failing.

Check Gateway and XS8 journals rather than restarting nginx automatically.

## 4. Preserve evidence before restarting

If the failure is reproducible, collect logs first:

```bash
mkdir -p ~/xs-recovery

journalctl -u kenwood-xs8 \
  -u kenwood-xs-gateway \
  -u kenwood-cec-xs-client \
  --since "30 minutes ago" \
  --no-pager > ~/xs-recovery/services.log

systemctl --no-pager --full status \
  kenwood-xs8 \
  kenwood-xs-gateway \
  kenwood-cec-xs-client \
  > ~/xs-recovery/status.txt
```

Useful things to record:

- What stopped working: Web, iPhone, CEC, or everything?
- Was it still reported active/running?
- Did restarting one service fix it?
- Did the full restart sequence fix it?
- Did any motor or CD transport command remain active?

## 5. Safety and project boundaries

- XS8 Service must remain the **sole GPIO owner**.
- Never run competing XS8 GPIO drivers.
- Avoid restarting while VOL or SEEK is being held.
- Do not restart the service during an active XS8 research experiment.
- Do not unmask the old `kenwood-xs-remote.service`.
- Do not deploy research-branch binaries as part of routine recovery.
- Do not reboot the entire Raspberry Pi unless service-level recovery fails and the cause has been investigated.

## 6. If the problem keeps returning

A service being `active/running` does not prove it can execute commands.

Possible future investigations:

- Gateway-to-XS8 connection recovery after inactivity.
- CEC client reconnection behavior.
- XS8 Service health versus actual command execution.
- systemd restart policies and watchdogs.
- Structured health checks that verify the command path without changing receiver state.

**Status:** Routine recovery procedure validated once, October 25, 2026. Root cause remains unknown.
