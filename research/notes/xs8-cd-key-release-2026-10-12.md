# CD transport KEY RELEASE: October 12 physical report

Target: Kenwood KR-V6070 with connected CD changer. This note records the operator-reported October 12, 2026 attended ROOT-context REPL experiments. **No hardware transmission was made while writing this note.** The reported physical outcomes are not independently replayable from archived raw edge CSV, per-frame service replies, or video; trial IDs are references to the operator's session, not archived capture files.

## Byte notation and evidence

Every TX byte below is **current logical TX** (LSB-first, short DATA LOW = 0, long DATA LOW = 1). Legacy TX is the older name for the same waveform, `logical = reverse8(~legacy)`. These TX conventions do not convert RX. No new RX value or receiver acknowledgment is claimed here. `TX_OK` would establish completed GPIO transmission, and waveform `MATCH` would establish a matching observation at the RX pins; neither alone proves Kenwood executed a command. The **physical PASS** labels below come from the operator's independent observation of changer behavior. The actual REPL edge records and service diagnostics for these trials were not deposited with this report. [Encoding history](../COMMAND_REGISTRY.md#byte-encoding-and-decoder-history)

| Logical ROOT TX | Legacy TX | Observed role and evidence limit |
|---|---|---|
| `0x41` | `0x7D` | CD PLAY indicator lit in the earlier physical trial; [prior report](xs8-root-validation-2026-10-12.md). |
| `0x42` | not recorded here | CD PAUSE in the historical bench map; no new physical trial reported here. |
| `0x9C` | `0xC6` | CD STOP/CANCEL: physical PLAY-to-STOP result in the [prior trial](xs8-root-validation-2026-10-12.md), and `FC → 9C` cancelled continuing track skip **and stopped playback** in the new report. |
| `0xFC` | `0xC0` | TRACK NEXT key down in a physically observed paired trial. Earlier tests described repeated/continuous next-track behavior when left active. |
| `0xEC` | `0xC8` | TRACK PREVIOUS key down in physically observed paired trials. |
| `0x6A` | `0xA9` | SEEK BACKWARD key down in a physically observed paired trial. Historical ROOT matrix labels `6A` `<<C`, continuous backward ([matrix legend and cell](kenwood-xs-reverse-engineering-v18.md#L230-L269)). |
| `0x7A` | `0xA1` | SEEK FORWARD key down in two physically observed paired trials. Historical ROOT matrix labels `7A` `>>C`, continuous forward ([matrix legend and cell](kenwood-xs-reverse-engineering-v18.md#L230-L269)). |
| **`0x72`** | **`0xB1`** | **CD transport KEY UP / RELEASE**: physically ended the active operations started by each of `FC`, `EC`, `6A`, and `7A` in the reported ROOT trials. This is a common release for those four operations, not a demonstrated universal XS8 release. |

The earlier [full ROOT+CD+playing sweep](kenwood-xs-reverse-engineering-v18.md#L210-L269) recorded no visible effect for logical `0x72`. That historical result remains valid for its baseline: no TRACK/SEEK operation was active. The new paired trials explain why a release-only action could have been invisible there; they do not prove `0x72` is inert in every other state.

## Paired physical observations

Each arrow means a separate XS8 frame sent directly in ROOT; no `0x17` context entry was used. Gaps are measured from the **end of one TX to the beginning of the next**. The operator requested 100 ms for the successful PRESS/RELEASE pairs and reported actual gaps of approximately 104–109 ms. Individual gap figures for each trial ID were not supplied. A requested 100 ms gap is a minimum under the chained REPL; verification, bus-free checks, and scheduling may extend it ([REPL chain behavior](../../docs/xs8-research-repl.md#chained-tx-v11)).

| Logical TX sequence | Trial reference | Operator-observed physical result |
|---|---|---|
| `FC → 72` | Earlier single/grouped trials; IDs not supplied | NEXT advanced one track at approximately 100 ms requested gap. A separate approximately 500 ms gap trial advanced two tracks. |
| `EC → 72` | Earlier trial; ID not supplied | PREVIOUS restarted the current track. |
| `EC → 72 → EC → 72` | `id=34` | Moved to the previous track after the repeated press/release sequence. The two `EC` actions were separated by an intervening `72`; do not conflate this with a single press. |
| `6A → 72` | `id=35` | Backward SEEK started and then terminated. |
| `7A → 72` | `id=36–37` | Forward SEEK started and then terminated; the pair was repeated twice. |
| `FC → 9C` | New trial; ID not supplied | Continuing NEXT/skip was cancelled, **and CD playback stopped**. `0x9C` is therefore not an equivalent transport key-up for a one-track action. |

The outcome is sensitive to the duration between PRESS and RELEASE: the reported `FC → 72` examples advanced one track near a 100 ms requested gap and two tracks near 500 ms. The approximately 104–109 ms actual gaps are observations from the successful 100 ms-request trials, **not** a hard maximum, a guaranteed one-track threshold, or a proven receiver timing limit. Playback position also matters for PREVIOUS: the first `EC → 72` restarted the current track, while a second press/release moved backward.

## Distinct termination actions and limits

- ROOT `0x72`: physical CD TRACK NEXT/PREVIOUS and SEEK BACKWARD/FORWARD release in the four tested operations.
- ROOT `0x9C`: CD STOP/CANCEL; it stopped playback in the `41 → 9C` and `FC → 9C` reports. Stopping a latched skip by stopping playback is not equivalent to KEY UP.
- Entered-context `0x09`: continuous volume STOP after `0xE9` or `0xF9` in the documented volume sequence.
- Entered-context `0x07`: returns to ROOT interpretation.
- ROOT `0x15`: releases the observed front-panel lock; after `0x17`, it has a tuner preset-scan effect.

No trial here establishes `0x72` behavior after `0x17`, with a different changer or receiver model, without an active transport operation beyond the earlier no-visible-effect baseline, or with a lost/invalid frame. The receiver's execution is inferred from the operator-observed physical action, not from a protocol acknowledgment. No generic KEY RELEASE across XS8 contexts or device classes has been demonstrated.

## Reproducing the documented form, not an unattended procedure

These examples express the reported logical-byte order in the existing local REPL. They are **not** executed by this document and require the service's explicit research TX/chain authorization, healthy monitor and safety interlocks, an operator at the changer, and an independent physical recovery method before a latched operation is attempted. The service may abort a chain after a TX or waveform failure, leaving the initial operation active; it does not automatically retransmit a release. Keep research authorization off in unattended operation. [REPL safeguards](../../docs/xs8-research-repl.md#chained-tx-v11)

```text
xs8> send 0xFC 0x72 --gap-ms 100 --observe-ms 1500
xs8> send 0xEC 0x72 0xEC 0x72 --gap-ms 100 --observe-ms 1500
xs8> send 0x6A 0x72 --gap-ms 100 --observe-ms 1500
xs8> send 0x7A 0x72 --gap-ms 100 --observe-ms 1500
```

Archive the actual `RESEARCH_TX_RECORD` times, waveform results, capture validity/gaps, and separately observed physical outcome in any future repetition. The reported approximately 104–109 ms gaps and physical PASS outcomes remain operator testimony until those underlying records are supplied.
