# XS8 Service receiver controls: VIDEO1/VIDEO2, TAPE2 MONITOR, POWER

These service commands back the Web Remote v2 controls ([Web API](web-api.md#web-remote-v2-controls)). All TX values are current logical TX (waveform_lsb). Every reply ends `receiver_execution=UNVERIFIED`: `TX_OK`/`MATCH` never proves the receiver acted, and nothing is retried automatically.

## CONTEXT keys: `SELECT_SOURCE VIDEO1|VIDEO2`, `TAPE2_MONITOR_TOGGLE`

One atomic CONTEXT transaction each, through VolumeControl's `context_key()`, the MUTE mechanism without MUTE's hold cancellation:

| Command | Frames | Evidence |
|---|---|---|
| `SELECT_SOURCE VIDEO1` | `0x17 → 0x78 → 0x07` | Physically validated 2026-10-12 (human-paced) |
| `SELECT_SOURCE VIDEO2` | `0x17 → 0x88 → 0x07` | Physically validated 2026-10-12 (human-paced) |
| `TAPE2_MONITOR_TOGGLE` | `0x17 → 0x48 → 0x07` | **Historical only**; not physically validated |

- Allowed only from volume IDLE with no STOP or CONTEXT obligation. Any volume hold answers `ERR VOLUME_BUSY`, any CD hold or pair `ERR CD_TRACK_BUSY`, an open research CONTEXT `ERR RESEARCH_CONTEXT_ACTIVE`, a CD fault `ERR CD_FAULT_LATCHED`; no frame is sent and no hold is cancelled.
- `context_may_be_open` is set before `0x17`. An unverified entry, key or exit frame latches `FAULT_LATCHED` after the `0x07` exit is attempted (three attempts); the key byte is never resent.
- Replies: `OK SOURCE_TX …` / `ERR SOURCE_UNCERTAIN …`, and `OK TAPE2_MONITOR_TX …` / `ERR TAPE2_MONITOR_UNCERTAIN …`, each with `gpio_tx` and `waveform` of the last frame. A shutdown signalled before the key byte answers `ERR SHUTDOWN` (any entry is undone by the exit).
- The MONITOR state is never inferred from a toggle. Passive tracking reports it from the receiver (`0xD7` ON, `0xE7` OFF).

## `POWER_ON` / `POWER_OFF`

| Command | Frame | Evidence |
|---|---|---|
| `POWER_ON` | ROOT `0x52` | Physically verified: powers the receiver on from standby (2026-10-12). Idempotence is not assumed |
| `POWER_OFF` | ROOT `0x62` | Physically verified: powers the receiver off (reported 2026-10-14). Idempotence is not assumed |

CONTEXT `0x39` (also historically a power-off) is never used. Both commands use the CD preflight, so they are refused during any volume or CD hold, any STOP, CONTEXT or CD release obligation, any fault, BOOT_INHIBIT and shutdown; an open research CONTEXT and an unhealthy monitor are refused too. Replies: `OK POWER_TX action=ON|OFF gpio_tx=… waveform=… receiver_execution=UNVERIFIED` or `ERR POWER_UNCERTAIN …`. An uncertain frame latches no fault (it leaves no release obligation). The service does not read back power state; the standby LED and switched outlets are not indicators.

Shutdown and an uncertain research TX block all of these commands in the service loop, as for CD commands.

Offline tests: `make test` (`volume_control_test` `test_context_key`, `xs8_service_core_test` `test_context_key_commands`, `test_power_commands`, `test_volume_key_hold`, and the socket tests).
