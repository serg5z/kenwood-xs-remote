# XS8 receiver readback: physical observations, 2026-10-11

Target: Kenwood KR-V6070. This note records the operator-reported October 11 physical tests of the service's `QUERY_STATUS` command and reviews older `0x14`/`0x15` evidence. The new tests were reported with decoded values but **no per-trial edge CSV, service reply log, or display photographs are archived here**. They are physical observations, not independently replayable capture evidence. The retained older serial scan is separately identified below.

## Byte notation and attribution

- All TX values called **logical** here use LSB-first, short DATA LOW = 0 and long DATA LOW = 1. Older **legacy TX** uses MSB-first, short LOW = 1 and long LOW = 0. The same TX waveform has `logical = reverse8(~legacy)`. Thus logical TX `0x13/0x14/0x15` corresponds to legacy TX `0x37/0xD7/0x57`, respectively. The older scan's *candidate* legacy TX `0x14/0x15` instead corresponds to logical TX `0xD7/0x57`. [Current conversion and TX routine](../sketches/xs-tx-test1.ino#L180-L235); [older scan TX routine](../sketches/xs-tx-scan2.ino#L190-L217).
- **Legacy RX** below is MSB-first, short LOW = 1, as in the later Arduino scanner; it is not silently converted as a TX byte. The current GPIO decoder reports both the waveform's `waveform_lsb` and `legacy_msb_short1` from the same edges. [Older scan RX routine](../sketches/xs-tx-scan2.ino#L306-L344); [GPIO decoder](../../xs8_decode.c#L84-L95).
- The `QUERY_STATUS` reply separates the observed local TX frame from later bus frames and calls their origin `UNATTRIBUTED`. The source and MONITOR names are interpretations of the observed bytes. Time ordering alone does not prove transmitter identity, receiver execution, or a formal XS8 message type. CD-player traffic can produce additional frames. [Service query classifier](../../xs8_query_status.cpp#L121-L173).

## Physical `QUERY_STATUS` observations

The operator issued one logical TX `0x13` query per trial in the tested ROOT state and observed the following source-associated legacy RX values. All six receiver sources were checked physically. "Passive" means a bus observation during a source change; "status" means the first subsequent frame associated in time with `QUERY_STATUS`. These are **reported physical observations**; raw TX/RX edge traces, timestamps, event-gap counts, and per-trial receiver display records were not supplied for archiving.

| Receiver source | Passive legacy RX (reported) | Passive `waveform_lsb` (derived) | First status legacy RX (reported) | Status `waveform_lsb` (derived) |
|---|---:|---:|---:|---:|
| TV2 | `0x71` | `0x71` | `0x11` | `0x77` |
| PHONO | `0x73` | `0x31` | `0x13` | `0x37` |
| TAPE1 | `0x79` | `0x61` | `0x19` | `0x67` |
| TV1 | `0x7A` | `0xA1` | `0x1A` | `0xA7` |
| TUNER | `0x7B` | `0x21` | `0x1B` | `0x27` |
| CD | `0x7D` | `0x41` | `0x1D` | `0x47` |

The `waveform_lsb` source values are **derived** from the established two-output decoder bit definitions, not separately archived October 11 readings. They are included to make a future raw-capture comparison possible; they must not be mistaken for logical TX commands or an additional RX frame. [Decoder bit assignments](../../xs8_decode.c#L84-L95).

For these six pairs, the directly checked **relationship between printed bytes** is `passive_legacy = status_legacy | 0x60`. This does not establish that the receiver internally applies that operation, nor does it extend the rule to other bytes or states. The same source/status pairs appear in the earlier written A/B tests. [Historical source table and passive broadcasts](kenwood-xs-reverse-engineering-reviewed.md#L113-L165).

The second status frame tracked the tested TAPE2 MONITOR setting:

| Visible MONITOR setting | Second status legacy RX | Same frame as `waveform_lsb` |
|---|---:|---:|
| OFF | `0x18` | `0xE7` |
| ON | `0x14` | `0xD7` |

The operator also observed passive bus traffic during MONITOR changes, but did not supply the passive byte values or raw edges. The status pair's OFF/ON distinction agrees with the historical MONITOR A/B record. [Historical A/B](kenwood-xs-reverse-engineering-reviewed.md#L126-L140). The RX `0x14` in this table is **not** logical TX `0x14`, and the `waveform_lsb` labels are two presentations of the same observed RX waveform, not separate frames.

CD DIRECT and SURROUND were each tested in ON and OFF states. No passive traffic was observed during those tested toggles, and neither setting changed the two known `0x13` response fields. This is a bounded negative observation: it does not rule out another command, another field, a delayed frame, or behavior in other receiver/CD states. It also does not make `0x14` or `0x15` a query for either feature.

**Validation status:** ROOT logical TX `0x13` now has operator-reported physical confirmation for six source associations and both MONITOR values on this KR-V6070. The exact captured edges and service replies from October 11 are not retained in the repository; response attribution remains temporal, and the names "source" and "MONITOR" remain interpretations grounded in the controlled display comparisons. No absolute volume, CD DIRECT, or SURROUND value was identified in these two frames.

## Historical `0x14` and `0x15` evidence

The older scan's `TX 0x57` is the **same waveform as today's logical TX `0x15`**. Its `TX 0x14` and `TX 0x15` candidate rows are different waveforms and must not be used as negative tests of today's logical `0x14`/`0x15`.

| Context and waveform | Recorded observation | Evidence and limit |
|---|---|---|
| ROOT, logical TX `0x14` = legacy TX `0xD7` | In the controlled ROOT+CD follow-up, legacy RX `0x1D`, then `0x18` at about +0.7/+109.8 ms, like `0x13` in that state. A separate lock test found that `0x14` did **not** release the locked front panel. | [Follow-up report](kenwood-xs-reverse-engineering-reviewed.md#L270-L272); [lock A/B](kenwood-xs-reverse-engineering-reviewed.md#L195-L206). The raw event log for the follow-up is absent; no cross-source or MONITOR-ON equivalence is established. |
| ROOT, logical TX `0x15` = legacy TX `0x57` | The retained legacy scan repeatedly used `0x57` as PREP/RECOVER and printed legacy RX `0x1D`, then `0x18` with CD selected. Later controlled tests found `0x15` released a persistent front-panel lock; when already unlocked it could still elicit the same pair. | [Retained scan protocol and first PREP/RECOVER](../captures/kenwood-xs-scan.txt#L29-L56); [later lock tests](kenwood-xs-reverse-engineering-reviewed.md#L177-L206); [unlocked observation](kenwood-xs-reverse-engineering%20copy%206.md#L430-L439). The scan's "source recovery" label is a historical interpretation, and its two frames do not show an additional status field. |
| After logical TX `0x17`, logical TX `0x14` / `0x15` | `0x14` selected tuner preset CH1; `0x15` started tuner preset scan. | [Targeted context tests](kenwood-xs-reverse-engineering-reviewed.md#L470-L485). These are documented side effects, not status-readback evidence. |
| Older scan candidate **legacy TX** `0x14` / `0x15` = **logical TX** `0xD7` / `0x57` | Each candidate had no printed RX; subsequent legacy TX `0xDE` still produced RX `0x7D` in the tested CD/source-control baseline. | [Actual candidate rows](../captures/kenwood-xs-scan.txt#L439-L476); [sketch sends candidate unchanged](../sketches/xs-tx-scan2.ino#L653-L664). They do not contradict the later logical `0x14`/`0x15` observations. RX silence is not evidence of no physical effect. |

The archive therefore supports only a **status-like two-frame response** for logical `0x14` in ROOT+CD and for logical `0x15` in the tested ROOT/CD conditions. Neither is established as an additional-state query. `0x15` has a known lock-release effect; both bytes have tuner side effects after `0x17`. The full newer ROOT sweep and follow-up did not retain labeled raw edge traces. The older scan is a serial log of decoded frames, not a raw waveform capture.

## Next experiment, if undertaken later

Prioritize archiving a single operator-controlled **ROOT `0x14`** comparison against `0x13` in one non-CD source with MONITOR OFF and ON. Record raw BUSY/DATA edges, decoded bytes under both conventions, validity/gaps, monotonic timestamps, visible source/MONITOR setting, and CD-player activity. Verify ROOT and an unlocked front panel before and after; stop if any unintended action occurs. This is justified by the ROOT+CD two-frame report, but `0x14` is **not proven read-only**. Do not use `0x15` as a routine probe: its documented lock-release action and context-sensitive preset scan make a transparent readback assumption unsafe. No such experiment was run for this note.

## Open questions

- Does logical ROOT `0x14` return the same pair as `0x13` for every source and both MONITOR settings, or only for the tested CD/OFF baseline? Does it have an unrecorded side effect?
- Why does logical ROOT `0x15` produce a status-like pair while also releasing the observed lock? Is it a general release/cancel action, or something else?
- Do either `0x14` or `0x15` report any *additional* receiver state in frames, timing, or conditions not present in the retained tests? None is established by the archive.
- Are CD DIRECT, SURROUND, absolute volume, power, or other settings readable through different XS8 commands? The tested `0x13` fields did not vary with CD DIRECT/SURROUND, and the archive establishes no absolute-volume field.
- Which device originated each status-like frame when CD-player traffic overlaps, and what observation window reliably separates unsolicited traffic from query-associated responses?
- Do the source-byte relation and MONITOR association hold outside the tested KR-V6070, context, power, and CD-player states? No broader compatibility has been validated.
