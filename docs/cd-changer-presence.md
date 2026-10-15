# Experimental CD changer presence probe (XS8 Service only)

`PROBE_CD_CHANGER` asks whether the CD changer answers on the XS8 bus. It is **experimental and default-off**: the service accepts it only with `--enable-cd-presence-probe`, and otherwise replies `ERR CD_PROBE reason=DISABLED` without transmitting. The Gateway, Web Remote and CEC client do not use it, and nothing sends it periodically, at startup or with SYNC.

## Evidence

ROOT `0xC3`, `0xD3` and `0xE3` have been followed by a valid `waveform_lsb=0x94` (legacy `D6`) about 3–5 ms after TX with the changer powered on, and by no response with it powered off. Repeated `0xC3` trials reproduced the on response. `0x94` also follows a CD STOP (about 320 ms later), the second CCRS and a physical STOP ([command matrices](xs8-command-matrices.md)), so it is interpreted only inside this transaction. No action of `0xC3` has been identified; it is sent in ROOT only.

## Transaction

The probe uses the QUERY_STATUS gates and slot: the receiver control must be `IDLE` with no volume or CD hold, no STOP or CD release owed, no CD fault, a closed context and a healthy monitor. Before transmitting, the service also requires:

- no local TX in the last **1 s** (`ERR CD_PROBE reason=RECENT_TX`);
- no bus frame in the last **500 ms** (`reason=BUS_ACTIVE`), so a delayed report from an earlier operation cannot land in the window;
- no research capture or chain (`reason=SESSION_BUSY`), no uncertain research TX (`reason=TX_UNCERTAIN_LATCHED`), no pipelined input, not shutting down.

It then sends exactly **one** ROOT `0xC3`, never retried. While it is pending (about 70 ms), every other command except `STATUS`, `RESEARCH_CONTEXT_STATUS` and `VOLUME_RELEASE` is refused with `ERR CD_PROBE reason=SESSION_BUSY`, and QUERY_STATUS and the probe exclude each other. Disconnecting the requesting client cancels the probe without changing the presence state. Frames in the window are withheld from passive receiver state.

The monitor history from a checkpoint before TX is classified against the TX start and completion times:

| Observation | Reply |
|---|---|
| Own valid `0xC3` echo, then exactly one valid `0x94` **1–20 ms** after TX completion and nothing else in the following **50 ms** quiet window | `OK CD_PROBE result=RESPONSE latency=<us> frames=1 rx=0x94@+<us> …` |
| Echo, then a clean silent window | `OK CD_PROBE result=NO_RESPONSE latency=NONE frames=0 rx=NONE …` |
| Echo, then a `0x94` outside 1–20 ms, any other byte, or more than one frame | `OK CD_PROBE result=AMBIGUOUS …` |
| Missing, invalid or mismatched echo; overlapping, invalid or incomplete frames; lost events; history overflow; monitor failure; uncertain TX | `ERR CD_PROBE reason=…` |

Every reply ends with `receiver_execution=UNVERIFIED`; `rx=` lists up to four frames with their offsets from TX completion. The windows are **provisional**.

## STATUS

Only with the flag, STATUS gains `cd_changer=PRESENT|NOT_DETECTED|UNKNOWN cd_changer_ns=<monotonic ns> cd_changer_last_probe=NONE|RESPONSE|NO_RESPONSE|AMBIGUOUS|FAILED`. Only RESPONSE sets PRESENT and only NO_RESPONSE sets NOT_DETECTED; AMBIGUOUS and FAILED update only `cd_changer_last_probe`. **NOT_DETECTED is not OFF**: silence is not established as a reliable absence signal, and ABSENT is never reported. Presence is independent of `receiver_source`: a CD source says which input the receiver reports, not whether the changer is powered.

**Gateway:** the Gateway accepts STATUS with or without these three fields. When present they must all be present, with a value from the lists above, `cd_changer_ns` a canonical unsigned 64-bit decimal, and a consistent combination: `UNKNOWN` exactly when `cd_changer_ns=0`, `last_probe=NONE` only with `UNKNOWN`, `RESPONSE` only with `PRESENT`, `NO_RESPONSE` only with `NOT_DETECTED`. Anything else, including an incomplete group or any other unknown field, makes the whole STATUS invalid (HTTP 503), as before. `GET /api/v1/status` adds a diagnostic object:

```json
"cd_changer": {"value": "NOT_DETECTED", "observed_mono_ns": 2000000000, "age_ms": 1234,
               "last_probe": "NO_RESPONSE", "diagnostic": true, "receiver_execution": "UNVERIFIED"}
```

It is `null` from a service without the fields. It never affects `receiver`, source selection, control eligibility or capabilities. The Gateway has no probe endpoint and never sends `PROBE_CD_CHANGER`; probes are still issued only over the local socket, in attended trials. The Web Remote does not display it.
