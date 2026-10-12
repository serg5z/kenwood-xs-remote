# XS8 research REPL v1

[`xs8_repl.py`](../xs8_repl.py) is a local, interactive client of the existing XS8 Service Unix socket. It never opens GPIO. The service remains the single GPIO owner and retains its CEC/Web command serialization, volume STOP obligations, watchdogs, waveform verification, and fault policy. Research TX is **off by default** and requires the separate `xs8-service --enable-research-tx` startup flag. That flag does not leave BOOT_INHIBIT: the existing `--allow-unrecovered-start` operator authorization and all IDLE/monitor/STOP/context checks still apply. Do not add the research flag to an unattended production service without a reviewed, attended research session.

From the repository root, run `python3 xs8_repl.py [--socket /tmp/kenwood-xs8.sock]`. Starting the REPL sends no XS8 command. Type `help` for `status`, `query`, `send 0xNN [milliseconds]`, `observe N`, `set observe N`, `history`, and `quit`. The default post-TX observation window is 1500 ms; the server accepts windows of 1–5000 ms. `query` invokes the existing `QUERY_STATUS` request and does not depend on research TX authorization. `observe` is passive and can operate with research TX disabled if the monitor is healthy.

Example transcript (illustrative; **not** a recorded hardware result):

```text
xs8> status
OK state=IDLE stop_owed=0 context_may_be_open=0 monitor=HEALTHY ...
xs8> set observe 1500
Default observation window: 1500 ms
xs8> observe 1000
OK RESEARCH_OBSERVE id=1 start_ns=... deadline_ns=...
OK RESEARCH_DONE id=1 count=0 ...
xs8> send 0x9C 1500
OK RESEARCH_TX gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED id=2 tx_start_ns=... tx_end_ns=... deadline_ns=... context=ROOT_ASSUMED
OK RESEARCH_RECORD id=2 index=0 kind=FRAME start_ns=... end_ns=... waveform_lsb=0x9C legacy_msb_short1=0xC6 valid=1 bits=8 lost_events=0 reason=NONE relation=TX_WINDOW origin=UNATTRIBUTED receiver_execution=UNVERIFIED
OK RESEARCH_DONE id=2 count=1 ...
```

`RESEARCH_SEND 0xNN N` takes a monitor checkpoint **before** its one logical TX frame. After the existing TX waveform check completes, the service returns an ID and drops the socket connection when the client closes it. The REPL polls `RESEARCH_READ ID INDEX` through short new connections, so it does not hold the service's one socket client for the 1–5000 ms observation window. A separate `STATUS` or other client can be served between polls; the service still serializes all TX on its existing control thread. If another client briefly owns the socket, the REPL retries only the read request for a bounded time; it never resends the research TX. Only one research observation session is active at a time. The most recent completed session remains readable until replaced by another; a new session cannot begin while one is active.

Each record preserves kernel monotonic start and end timestamps, `waveform_lsb`, `legacy_msb_short1`, validity, bit count, decoder reason, and lost-edge count for a gap. `relation=TX_WINDOW` means only that its start fell inside the local TX timing window. `origin=UNATTRIBUTED` remains explicit for every bus frame; later traffic is not called an acknowledgment. `gpio_tx=TX_OK` reports GPIO completion, `waveform=MATCH` reports a matching observed bus frame, and `receiver_execution=UNVERIFIED` states the remaining limit. A completed frame extending beyond the observation deadline is reported incomplete. Monitor failure, history wrap, and the 256-record session cap are reported by `monitor_failed=1` or `overflow=1`; records are never silently presented as a complete capture after those faults.

The service rejects research TX in BOOT_INHIBIT, FAULT_LATCHED, active volume, an unhealthy monitor, or an unresolved STOP/context state. It also rejects `0x17` in v1, because that known byte enters a different command context and would undermine the promised ROOT workflow. Other unknown ROOT bytes can still change state or latch a device; `context=ROOT_ASSUMED` is an explicit assumption, not feedback from the receiver. A failed or mismatched research TX blocks further research TX until the service is investigated and restarted. There is no automatic retransmission, sweep, or cleanup command for an arbitrary research byte. The service logs research SEND, OBSERVE, and CANCEL requests to stderr. Ctrl+C while polling sends `RESEARCH_CANCEL ID` to stop collecting that observation; if pressed during a SEND request, the service may already have transmitted the frame. Cancellation does not cancel an XS8 operation in progress.

The proposed first physical validation is a single ROOT `0x9C` during ordinary CD playback, with the receiver and changer attended. Before any TX, confirm a reachable physical CD STOP and independent changer power removal path; the archive has **not** established a guaranteed recovery from a latched TRACK/SEEK operation. Record the changer's physical stop separately from `TX_OK`, waveform match, and any RX `D6`. Do not test `0xFC → 0x9C` until physical recovery has been demonstrated. See the [CD transport audit](../research/COMMAND_REGISTRY.md#L50-L69) for historical command evidence. No physical test is run by this implementation.

## Local protocol extension

All replies remain one line. `RESEARCH_OBSERVE N` starts a passive session. `RESEARCH_SEND 0xNN N` starts one TX and post-TX observation. The response includes `id`, timing, GPIO TX result, and waveform result; an uncertain TX returns `ERR RESEARCH_TX` **with an ID** so its captured bus activity can still be read. `RESEARCH_READ ID INDEX` returns one `RESEARCH_RECORD`, `RESEARCH_WAIT` while collection continues, or `RESEARCH_DONE` with record count and integrity flags. `RESEARCH_CANCEL ID` ends observation without any TX. Requests use separate connections and do not replay a command. Malformed requests return `ERR RESEARCH reason=INVALID_REQUEST`. The existing `STATUS`, `QUERY_STATUS`, volume, source, and MUTE wire formats are unchanged.

## Offline checks and Pi validation

Run `make test` and `python3 -m unittest tests/test_xs8_repl.py` with no receiver connected. On the Raspberry Pi, build with `make .build/xs8-service`; validate the CLI first against a fake Unix-socket service. Before enabling research TX on the real XS8 Service, review its effective startup command, socket ownership, monitor health, receiver state, and physical recovery plan. Keep the research flag absent from normal startup, and remove it again after the attended session. The current implementation is not deployed by this document.
