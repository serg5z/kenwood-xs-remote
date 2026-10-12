# XS8 KEY RELEASE candidate shortlist

Research review, 2026-10-12, updated after the October 12 physical CD transport trials. **No command was transmitted while writing or updating this note.** All bytes below are **logical TX** bytes, not received bytes. For a documented legacy TX label of the *same waveform*, `logical = reverse8(~legacy)`. RX values retain their original decoder notation; neither this extraction nor the proposed tests convert RX with the TX formula. See the [registry's decoder history](../COMMAND_REGISTRY.md#byte-encoding-and-decoder-history).

## Scope and evidence limit

The original [union matrix](../XS8_COMMAND_COVERAGE.md#3-union-command-establishment-and-unknown-candidates) labeled **170** bytes `T`. October 12 physical trials established logical ROOT TX **`0x72` (legacy `0xB1`) as a CD transport release for `0xFC/0xEC/0x6A/0x7A`**, moving it from `T` to `E`. The current union has **169** `T` bytes: each was sent in the *reported* ROOT sweep, but neither ROOT nor the selective `0x17` tests established a command effect. `T` does **not** mean electrically silent, harmless, or a NOP. The ROOT baseline was CD selected and playing, with the source and lock restored between trials; no TRACK/SEEK operation was active when the sweep assessed `0x72`. Its prior no-effect result remains historical, while the new active-operation result explains why RELEASE was invisible then ([paired physical report](xs8-cd-key-release-2026-10-12.md), [ROOT matrix and follow-up](kenwood-xs-reverse-engineering-v18.md#L250-L282)). The sweep's per-byte raw log and new trial logs are not archived, so these are operator-reported hardware results rather than independently replayable capture evidence.

Among the remaining 169, the entered-context matrix has 82 `S` (explicitly tested **without RX**, not necessarily without action), 3 `T` (no visible effect recorded), and 84 `U` (individual test not documented). The removed `0x72` is an entered-context `U`: its release action has been validated only in ROOT. These are **context evidence counts**, not three safety classes. A candidate can have unobserved state-dependent side effects. The partial retained [legacy serial scan](../captures/kenwood-xs-scan.txt#L499-L515) stops at legacy candidate `0x17` and cannot substantiate the later full sweep; its old TX byte labels must be converted before comparison. An RX frame is never a candidate TX byte merely because its number matches.

## Current 169 ROOT-tested, unestablished bytes

These are exactly the union matrix `T` cells, grouped by high nibble for audit. They are **not** an instruction to scan or transmit a range.

| High nibble | Logical TX bytes (`0x` prefix applies to each) | Count |
|---|---|---:|
| `0` | `00 01 02 03 06 0A 0B 0C 0D 0E 0F` | 11 |
| `1` | `10 11 12 16 19 1A 1B 1C 1D 1E 1F` | 11 |
| `2` | `20 22 23 26 27 29 2A 2B 2C 2D 2E 2F` | 12 |
| `3` | `30 32 33 37 3A 3B 3C 3D 3E 3F` | 10 |
| `4` | `40 43 47 4A 4B 4C 4D 4E 4F` | 9 |
| `5` | `50 53 57 59 5A 5B 5C 5D 5E 5F` | 10 |
| `6` | `60 63 6B 6C 6D 6E 6F` | 7 |
| `7` | `70 73 79 7B 7C 7D 7E 7F` | 8 |
| `8` | `80 87 89 8B 8C 8D 8E 8F` | 8 |
| `9` | `90 91 92 93 95 96 97 99 9A 9B 9D 9E 9F` | 13 |
| `A` | `A0 A2 A3 A4 A7 A8 AA AB AC AD AE AF` | 12 |
| `B` | `B0 B1 B2 B3 B5 B7 B8 B9 BA BB BD BE BF` | 13 |
| `C` | `C0 C1 C2 C4 C5 C6 C7 C9 CA CB CD CE CF` | 13 |
| `D` | `D0 D1 D2 D4 D7 D9 DB DC DD DE DF` | 11 |
| `E` | `E0 E1 E2 E7 E8 EB ED EE EF` | 9 |
| `F` | `F0 F2 F3 F5 F6 F7 F8 FA FB FD FE FF` | 12 |
| **Total** | | **169** |

## Exclusions and risk triage

The `T` extraction excludes known-action and ambiguous union cells by construction. In particular, **do not reinsert** established CD release `0x72`; context entry/exit `0x17/0x07`; power `0x52/0x62` or context `0x39`; continuous volume `0xE9/0xF9` and its context STOP `0x09`; continuous CD controls `0x6A/0x7A/0xEC/0xFC`; front-panel lock setters, tuner scan/submode controls, or other potentially latched commands. ROOT `0x9C` is an *already established CD STOP result* and therefore outside the 169; ROOT `0x15` is an observed lock release but has a tuner preset-scan effect after `0x17`. `0xF1` and `0xC3/0xD3/0xE3` are union `A`, not `T`: the former has conflicting STOP reports, and the latter produce unexplained RX `0xD6`. None qualifies as a low-risk unknown candidate ([registry CD and context rows](../COMMAND_REGISTRY.md#tx-registry-cd-and-changer-actions), [release comparison](../XS8_COMMAND_COVERAGE.md#generic-key-release-hypothesis)).

Within the remaining 169, **risk is unknown**, not low. The following triage preserves the pre-discovery candidate priorities:

| Triage | Bytes | Reason |
|---|---|
| Documented negative follow-up; possible release-only candidates | Logical `0x32`, `0x4C`, `0x50` (legacy TX `0xB3`, `0xCD`, `0xF5`) | Each was individually sent in ROOT+CD, had no RX and no reported visible effect; a subsequent `0x13` still returned status. No TRACK/SEEK was active ([follow-up](kenwood-xs-reverse-engineering-v18.md#L270-L276)). This is useful baseline evidence, not proof of safety or release behavior. |
| Negative in two surveyed contexts; retain as secondary research candidates | Logical `0xA4`, `0xC4`, `0xD4` (legacy TX `0xDA`, `0xDC`, `0xD4`) | ROOT sweep shows no visible effect; the entered-context tuner-family pass also reported no visible effect ([ROOT matrix](kenwood-xs-reverse-engineering-v18.md#L250-L269), [tuner table](kenwood-xs-reverse-engineering-v18.md#L380-L417)). Tuner submode dependence remains possible, so do not batch or treat them as inert. |
| Deprioritize for the first CD release trial | `0x1F`, `0x57` | Logical `0x1F` did not exit `0x17` context; logical `0x57` did not release the front-panel lock ([controlled negatives](kenwood-xs-reverse-engineering%20copy%207.md#L61-L80)). Those failures do **not** rule out CD release, but neither supplies supporting evidence. Legacy TX `0x57` is **logical `0x15`**, not logical `0x57`; the old scan's legacy candidate `0x15` is logical `0x57` and printed no RX ([decoder review](xs8-readback-2026-10-11.md#L41-L52)). |
| Defer | Remaining 161 `T` cells | The reported ROOT sweep found no established action, but no candidate-specific release evidence or controlled TRACK/SEEK-off follow-up distinguishes these bytes. Entered-context `S` means no RX, and `U` means no individual test. Numeric proximity to STOP or START codes is not evidence. |

The first two rows preserve **pre-discovery investigation priorities**, not a current recommendation to test more unknown release bytes. No positive KEY RELEASE evidence exists for any of the remaining 169. The `0x72` discovery shows how an idle-sweep `T` could hide a release action; it does not establish such behavior for another `T` cell.

## Resolved lead and distinct known actions, outside this shortlist

ROOT `0x72` is the **resolved CD transport lead**: the reported `FC/EC/6A/7A → 72` pairs physically ended all four active operations. The earlier no-effect dot for `0x72` in ROOT+CD+playing remains a correct description of the no-active-operation baseline ([paired report](xs8-cd-key-release-2026-10-12.md), [old matrix](kenwood-xs-reverse-engineering-v18.md#L250-L269)). The historical map said ROOT `0x9C` stopped continuous forward skip after `0xFC`; a new `FC → 9C` trial confirmed skip cancellation **with playback STOP**, consistent with `0x9C` as CD STOP/CANCEL rather than KEY UP ([older report](kenwood-xs-reverse-engineering%20copy%207.md#L437-L445), [new physical report](xs8-cd-key-release-2026-10-12.md)). Context `0x09` stopped active volume in a tested `0x17 → 0xE9 → 0x09 → 0x07` sequence; `0x07` exits that context; `0x15` releases a different front-panel lock. Their observed effects remain distinct ([context control](kenwood-xs-reverse-engineering-v18.md#L553-L562), [registry sequence](../COMMAND_REGISTRY.md#documented-sequences)).

No retained passive capture isolates a physical CD TRACK/SEEK **button-up** frame. Passive CD status/event RX values cannot be promoted to TX RELEASE opcodes ([passive button tests](kenwood-xs8-sniffing-notes.md#L49-L103)).

## Original candidate test plan — historical proposal, now superseded for CD release

1. **Resolve the recovery prerequisite first.** With an operator at the changer, verify an independent physical way to stop a continuous TRACK/SEEK operation in the intended starting state (for example, a reachable front-panel control or a known safe power isolation procedure). Record the actual recovery action and outcome. The `0x41 → 0x9C` STOP result alone does not validate recovery during a latched `0xFC`/`0xEC` run. Do not start a latched trial until physical recovery is demonstrated.
2. **Characterize one candidate at a time in ROOT.** Confirm service IDLE, healthy monitor, no STOP obligation/open context, selected CD, and a known playback state. Use one single-byte REPL `send 0xNN --observe-ms 1500` for one candidate from `0x32/0x4C/0x50`. Archive requested logical TX, any legacy TX label, actual TX timestamps, waveform validity/gaps, raw observations, and visible receiver/changer state. Do not use RX silence as a NOP verdict. Restore the baseline before another candidate. The individually documented ROOT+CD follow-up makes these useful controls, but it does not guarantee benign effects in another state.
3. **Test a known-action comparator before an unknown.** The original proposal was `send 0xFC 0x9C --gap-ms 200 --observe-ms 1500` after physical recovery was established. An attended `FC → 9C` trial was subsequently reported: skip stopped **and playback stopped** ([new report](xs8-cd-key-release-2026-10-12.md)). The proposed 200 ms example is not the recorded gap for that trial; do not present it as measured data.
4. **Only then consider one unknown candidate per latched trial.** If the comparator and physical recovery are reliable, choose one of `0x32/0x4C/0x50`, review its single-byte result and possible state interactions, then consider one attended `send 0xFC 0xNN --gap-ms 200 --observe-ms 1500`. A candidate may be a stateful control, not a release; its action after `0xFC` may differ from its idle action. Never chain multiple unknown candidates, repeat a trial automatically, or interpret a matching TX waveform as receiver execution. Stop this line of tests after any unexpected state change, monitor gap, uncertain TX, or failed recovery.
5. **Broaden only with new evidence.** `0xA4/0xC4/0xD4` require separate tuner-state interaction review before any CD trial. The other 161 have no evidence-based ordering. This search is no longer needed to establish the four tested CD releases. A generic XS8 KEY RELEASE claim would still require tests in other contexts and devices, with physical effects and raw bus records archived.

The chained REPL uses one service-side checkpoint and a gap measured **from the end of one TX to the start of the next**. A requested gap is a minimum, not a hard maximum; waveform verification, bus-free checks, and scheduling can lengthen it. Research TX and chain authorization are default-off, and the service retains its interlocks ([REPL chain documentation](../../docs/xs8-research-repl.md#chained-tx-v11)). This historical plan sends nothing now. `0x72` is established only for the tested ROOT CD transport operations, and no remaining candidate byte is established as safe or context-independent.
