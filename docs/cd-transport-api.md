# CD track socket API, phase 1

`xs8-service` remains the only XS8 GPIO owner. This phase adds two **local Unix socket** commands. It does not add a Web endpoint, a held SEEK operation, or a CD STOP command. The [protocol evidence](xs8-protocol.md#cd-transport-press-and-release-in-root) is specific to the KR-V6070 and connected changer.

| Request line | Service-owned ROOT frames | Meaning in the tested setup |
|---|---|---|
| `CD_TRACK_NEXT\n` | logical `0xFC → 0x72` | One discrete NEXT press/release attempt. |
| `CD_TRACK_PREVIOUS\n` | logical `0xEC → 0x72` | One discrete PREVIOUS press/release attempt; it may restart the current track. The service never sends a second pair automatically. |

The service completes both frame attempts before replying. After a verified PRESS, it requests a **100 ms minimum gap from the recorded end of KEY DOWN TX to the beginning of KEY UP TX**. Waveform verification, BUSY-free checks, and scheduling can make the actual gap longer. If PRESS transmission or waveform verification is uncertain, RELEASE is attempted immediately. No `0x17` context entry or `0x9C` CD STOP is substituted.

On a fully matched pair, a reply has this form:

```text
OK CD_TRACK_TX action=NEXT press_gpio_tx=TX_OK press_waveform=MATCH release_gpio_tx=TX_OK release_waveform=MATCH cd_result=TX_VERIFIED receiver_execution=UNVERIFIED
```

`PREVIOUS` uses `action=PREVIOUS`. `TX_VERIFIED` means both GPIO transmissions completed and their bus waveforms matched; it is **not** an acknowledgment from the receiver or a guaranteed track movement. An uncertain pair returns `ERR CD_TRACK_UNCERTAIN` with the same evidence fields and `cd_result=PRESS_UNCERTAIN` or `RELEASE_UNCERTAIN`. A failed PRESS still triggers one RELEASE attempt. A failed or missing RELEASE match latches `cd_state=FAULT_LATCHED` and retains `cd_release_owed=1` until a confirmed transmission or process restart. A PRESS failure also latches the CD fault even when RELEASE matches, because the operation's outcome remains uncertain.

`STATUS` adds `cd_state=IDLE|ACTIVE|FAULT_LATCHED`, `cd_release_owed=0|1`, and `cd_last_result=NONE|TX_VERIFIED|PRESS_UNCERTAIN|RELEASE_UNCERTAIN`. A CD fault also reports the existing overall `state=FAULT_LATCHED` so Gateway readiness cannot mistake it for ordinary IDLE. Other status fields remain. New CD requests require a connected local client, healthy monitor, receiver control `IDLE`, no volume STOP obligation, and no open `0x17` context. Shutdown and an earlier uncertain research TX also block new CD presses. A CD fault blocks further active commands and research TX; `STATUS` remains readable and a pending volume RELEASE remains permitted. A command arriving during the pair receives `ERR CD_TRACK_BUSY`. A research chain already in progress rejects CD commands under its existing session-busy rule. The single owner thread sends no unrelated frame between KEY DOWN and KEY UP.

## Failure, shutdown, and operator recovery

The release obligation is set **before** KEY DOWN may reach the bus. The service attempts `0x72` after a failed or uncertain PRESS and after an interrupted wait. If the first RELEASE is unconfirmed, a graceful shutdown makes at most one additional RELEASE attempt. Failure leaves a visible fault and a prominent log warning. The service does **not** automatically send `0x9C`, repeat the track command, or interpret `TX_OK`/`MATCH` as physical execution.

`SIGINT`/`SIGTERM` request shutdown through the normal control thread; a signal during the pair does not intentionally skip RELEASE. Process crash, `SIGKILL`, power loss, failed GPIO hardware, lost ground, or a receiver that ignores a correctly observed waveform can still leave transport motion active. Software cannot guarantee physical termination in those cases.

After any `CD_FAULT_LATCHED`, the operator must **physically stop and inspect the changer**, restore a known receiver/CD state, and inspect the service's release evidence before restarting. The fault is deliberately not clearable over the socket. A restart begins in `BOOT_INHIBIT`; leaving it requires the existing explicit `--allow-unrecovered-start` startup authorization. That flag is an operator assertion after physical recovery, not proof supplied by hardware. Do not configure unattended automatic fault clearing or restart as a substitute for inspection. A process restart loses the in-memory release obligation, so the physical check also applies after an unexpected termination.

The service executes a pair synchronously on its control thread. Other socket requests may be delayed during the pair, including CEC/Web requests, but a CD pair is refused while volume control is active. The nominal delay is 100 ms plus two frame transmissions and waveform checks; Linux scheduling and BUSY waits have no hard maximum. The release attempt still occurs after a slow PRESS verification. This phase has no receiver-side completion acknowledgment and no automatic track retry.
