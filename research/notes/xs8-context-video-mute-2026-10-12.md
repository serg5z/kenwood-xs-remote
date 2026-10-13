# CONTEXT VIDEO1, VIDEO2 and MUTE: physical validation, 2026-10-12

Target: Kenwood KR-V6070. These attended trials used the [interactive CONTEXT research session](../../docs/xs8-research-repl.md#interactive-context-research-session-attended-default-off) (`context enter` → `send 0xNN` → `context exit`) and the existing ROOT research send. This note records the operator-supplied results. **No hardware transmission was made while writing it.** Raw REPL output, service journal lines and passive capture records were summarized by the operator; they are not archived in this repository.

All TX values are **current logical TX** (LSB first, short DATA LOW = 0, long DATA LOW = 1). Sequences such as `0x17 → 0x78 → 0x07` mean separate XS8 frames sent through the service-owned transmitter (BUSY check and waveform verification on each). The operator paced them interactively (enter, then send, then exit), so the gaps between frames were human-scale. They were **not** back-to-back like the production MUTE path.

## Evidence levels

The results below keep three kinds of evidence apart:

1. **Electrical TX verification:** `gpio_tx=TX_OK` and `waveform=MATCH`. The service completed the frame and observed a matching waveform on the bus. This proves **nothing** about receiver execution (`receiver_execution=UNVERIFIED`).
2. **Inferred QUERY responses:** ROOT `QUERY_STATUS` (`0x13`) labels such as `source_inferred=TV1`. These are the service's interpretation of the two receiver RX frames, using the existing [source and MONITOR mapping](kenwood-xs-reverse-engineering-reviewed.md). They are receiver reports, not proof of a physical routing change. The raw RX byte values were not included in the operator summary.
3. **Observed receiver behavior:** what the operator saw or heard directly: the displayed input, audio returning, front-panel response. Only this level establishes physical behavior.

## Confirmed CONTEXT sequences

| Logical TX sequence | Observed receiver behavior (operator) | Exit evidence (electrical) | ROOT QUERY afterwards (inferred) |
|---|---|---|---|
| `0x17 → 0x78 → 0x07` | **Selected VIDEO1 / TV1** | `0x07` `TX_OK`/`MATCH`, 1 attempt, `context_may_be_open=0` | 2 valid frames, no capture gaps: `source_inferred=TV1`, MONITOR OFF |
| `0x17 → 0x88 → 0x07` | **Selected VIDEO2 / TV2** | same | 2 valid frames, no capture gaps: `source_inferred=TV2`, MONITOR OFF |
| `0x17 → 0xD8 → 0x07` (MUTE already active) | **Audio restored (unmute)** | same | 2 valid frames, no capture gaps: `source_inferred=CD`, MONITOR OFF |

The operator summary gave the exit evidence explicitly. Per-frame `TX_OK`/`MATCH` results for the `0x17` entry and the candidate frames were not separately restated.

**MUTE is a toggle.** The earlier attended production MUTE trial (`0x17 → 0xD8 → 0x07`, sent back-to-back by the service) physically muted. In this trial, the same logical sequence physically restored audio while MUTE was active. Together these establish **toggle** behavior for this receiver and state. They do not establish a separate mute-on or mute-off command, and the service cannot read the current mute state.

## Control: ROOT `0x78`

With **TUNER** selected, a ROOT research send of `0x78` (no `0x17` entry) did **not** switch the receiver to VIDEO1. No other effect was reported. This matches the controlled ROOT+CD sweep, where `0x78` is direct CD track 7 and the track family can be silent away from CD. It also matches the historical "NORMAL"-state labels VIDEO1 (`E1` → `0x78`) and VIDEO2 (`EE` → `0x88`), which were most likely observed inside the `0x17` context. Context `0x08` (PHONO) was already recorded in the [command registry](../COMMAND_REGISTRY.md). The x8-column bytes therefore behave as input selection in context and as CD track selection in ROOT+CD. Only `0x08`, `0x78` and `0x88` have been physically confirmed in context; `0x38` (historical TAPE1) and `0x48` (historical MONITOR toggle) remain untested here.

## Recovery checks after each experiment

All three independent checks passed after **each** of the three experiments:

1. **ROOT query:** `QUERY_STATUS` returned two valid frames that matched the physically observed state (table above).
2. **Front panel:** front-panel **TUNER** selection worked, so the controls were not locked.
3. **ROOT source command:** production `SELECT_SOURCE CD` visibly selected **CD**, not tuner preset CH8, so the receiver was interpreting commands in ROOT again.

These checks are the evidence that `0x07` restored normal operation **after these three specific bytes** in the tested states. They do not show that `0x07` recovers from arbitrary CONTEXT bytes.

## Interactive session lifecycle validation

The attended runs exercised the full interactive lifecycle on hardware: `context enter`, one `send` per experiment, explicit `context exit` with a service-sent `0x07` (one attempt, `TX_OK`/`MATCH`, `context_may_be_open=0`), then `context verify`. No fault latched. Lease expiry, owner-disconnect exit, shutdown exit and failure paths were **not** exercised physically; they remain covered only by the offline tests.

### Open anomaly: `NOT_ACTIVE` after the initial empty trial

During the first trial, `context enter` immediately followed by `context exit` with no byte sent, the REPL printed a `NOT_ACTIVE` message. For that same exit, the service journal reported `reason=CLIENT_EXIT` and successful cleanup (`0x07` verified, `context_may_be_open=0`).

**The cause is not established.** One explanation, from reading the code rather than from evidence, is a race in the console. The background heartbeat may have sent `CONTEXT_SESSION_HEARTBEAT` on the owner connection just after the service processed EXIT and before the console stopped the heartbeat thread. The service correctly answers `NOT_ACTIVE` to a heartbeat for a closed session, and the thread prints that reply. If that is the cause, the message is cosmetic and does not affect the service-side exit. It needs to be confirmed, for example with the exact REPL transcript and its timestamps, before any change is made.

## Future implementation candidates (not implemented)

None of the following exists as a production feature. Each needs its own design, review, tests and attended validation:

- **Production VIDEO1/VIDEO2 source selection:** a fixed service-owned `0x17 → 0x78 → 0x07` or `0x17 → 0x88 → 0x07` transaction, like MUTE. It would track `context_may_be_open` and fault on an unverified exit. TV1 and TV2 stay blocked in [source selection v1](../../docs/source-selection.md). Open questions:
  - whether back-to-back timing (as in the production MUTE path) behaves like these human-paced trials;
  - behavior from inputs other than those tested;
  - whether CD playback or other states change the result.
- **MUTE toggle semantics:** the production `MUTE` command and Web MUTE label should describe a **toggle**, not "mute". Any explicit mute-on/mute-off or state display needs a reliable mute-state readback, which is not available.
- **TAPE2 MONITOR (`0x48` in context)** and **TAPE1 (`0x38` in context):** historical labels only; untested.
