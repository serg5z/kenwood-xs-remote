# XS8 logical-TX command coverage and KEY RELEASE research

This is a consolidated **logical TX** evidence map for the KR-V6070, not a list of safe commands. The ROOT reference is the reported 2026-10-09 full `00–FF` sweep with CD selected and playing; its per-byte raw log was not archived ([v18 baseline and matrix](notes/kenwood-xs-reverse-engineering-v18.md#L210-L276)). The entered-context reference is a selective set of tests after logical TX `0x17`, not a full sweep ([v18 context map](notes/kenwood-xs-reverse-engineering-v18.md#L508-L562)). New physical results are [recorded separately](notes/xs8-root-validation-2026-10-12.md), without raw edges or service replies. The corrected v18 ROOT cells for `0x41` and `0x52` are preserved alongside the older no-effect observations.

## Notation and evidence levels

Each row/column is logical TX `0xHL` (high nibble row, low nibble column). Legacy TX is another label for the **same waveform**, converted by `logical = reverse8(~legacy)`: `B5 → 52`, `7D → 41`, `C6 → 9C`, `57 → 15`, `1F → 07`, `60/68 → F9/E9`. Never apply that conversion to RX. Later-decoder legacy RX labels such as `D6`, `1D`, and `18` remain as printed; early passive RX labels require their own convention ([registry encoding history](COMMAND_REGISTRY.md#byte-encoding-and-decoder-history), [original TX routine](sketches/xs-tx-test1.ino#L180-L235), [later RX routine](sketches/xs-tx-test2.ino#L349-L355)). The retained [partial scan](captures/kenwood-xs-scan.txt#L29-L56) uses legacy **TX** labels and ends at candidate legacy `17`, so its candidate labels must be converted before matrix placement.

- `V` — operator-reported **physical verification** in a controlled state. It is not independently replayable without raw trial records. ROOT `13` has the [October 11 readback report](notes/xs8-readback-2026-10-11.md); `41/52/9C` have the [October 12 report](notes/xs8-root-validation-2026-10-12.md); `82–85` were physically reported in the [earlier four-input validation](notes/xs8-root-validation-2026-10-12.md), but its per-trial log is not in this archive. `V` does not mean universal safety or receiver acknowledgment.
- `R` — behavior or RX **reported in historical notes**, without a retained per-byte raw log. The presence of RX can be the only observed result; semantics may remain unknown.
- `K` — **known protocol/control byte** in the tested context: ROOT `15` releases a control lock and ROOT `17` enters an alternative interpretation; after `17`, `07` exits and `09` stops an active volume hold. These are observed control effects, not claims of a universal release opcode.
- `T` — **tested with no visible effect recorded** in the specified state. It does not mean NOP or no RX unless a separate no-RX test says so.
- `S` — **explicitly tested without RX**, but physical effects were not established by that negative result. This is used in the selective context map so no-RX is never silently equated with no action.
- `A` — **ambiguous or conflicting evidence**. See the byte-specific notes below; a different state may explain a discrepancy.
- `U` — **not documented as individually tested** in that context. A summary dot from a selective map is insufficient to turn it into `T`.
- `!` suffix — **known potential side effect or latched behavior**: power-off, control lock, context entry, continuous CD operation, tuner submode/scan, or continuous volume. It marks a conservative hazard, not an exhaustive safety classification. A cell such as `R!` retains its evidence level.

## 1. ROOT matrix

Baseline is ROOT + CD selected + CD playing, with source and lock state restored between trials. `V` for `41` and `52` overlays newer physical tests in different initial states; their earlier no-visible-effect observations remain recorded below. `A` preserves historical NORMAL attributions and the `F1` STOP discrepancy rather than selecting a global meaning.

| hi\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** | T | T | T | T | T | T | T | T | A | T | T | T | T | T | T | T |
| **1** | T | T | T | V | R | K | T | K! | A | T | T | T | T | T | T | T |
| **2** | T | R | T | T | T | T | T | T | R | T | T | T | T | T | T | T |
| **3** | T | R | T | T | T | R! | R! | T | A | T | T | T | T | T | T | T |
| **4** | T | V | R | T | T | R! | R! | T | A | A | T | T | T | T | T | T |
| **5** | T | R | V | T | T | R! | R! | T | R | T | T | T | T | T | T | T |
| **6** | T | R | R! | T | T | R! | R! | R! | R | A | R! | T | T | T | T | T |
| **7** | T | R | T | T | A | R! | R! | R! | A | T | R! | T | T | T | T | T |
| **8** | T | R | V | V | V | V | R | T | A | T | R | T | T | T | T | T |
| **9** | T | T | T | T | T | T | T | T | R | T | T | T | V | T | T | T |
| **A** | T | R | T | T | T | R! | R! | T | T | T | T | T | T | T | T | T |
| **B** | T | T | T | T | T | T | R | T | T | T | T | T | R | T | T | T |
| **C** | T | T | T | R | T | T | T | T | A | T | T | T | R | T | T | T |
| **D** | T | T | T | R | T | R! | R! | T | T | T | R | T | T | T | T | T |
| **E** | T | T | T | R | T | R | R | T | T | T | R | T | R! | T | T | T |
| **F** | T | A | T | T | A | T | T | T | T | T | T | T | R! | T | T | T |

**ROOT evidence.** `13` produced two source/MONITOR-associated RX frames; the six sources and both MONITOR states were later physically compared ([query A/B](notes/kenwood-xs-reverse-engineering-reviewed.md#L113-L175), [physical readback](notes/xs8-readback-2026-10-11.md)). `14` produced the same `1D,18` pair in ROOT+CD but is not proven equivalent elsewhere. `15` released the persistent observed lock; `17` changed interpretation ([lock and context tests](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L206), [context A/B](notes/kenwood-xs-reverse-engineering-reviewed.md#L410-L428)).

The reported CD actions include direct tracks `18/28/38/48/58/68/78/88/98`, STOP `21/31/51/61/71/A1/9C`, PAUSE `42`, PLAY/PAUSE toggle `BC`, next disc `8A`, continuous `6A/7A/EC/FC`, repeat `CC`, CCRS `B6`, restart `E5/E6`, and time display `EA` ([ROOT sweep details](notes/kenwood-xs-reverse-engineering-v18.md#L270-L378)). The new trial physically observed `41` PLAY and `41 → 9C` STOP; `52` powered on a standby receiver while its CD changer powered through the switched AC outlet ([physical report](notes/xs8-root-validation-2026-10-12.md)). `41` had no reported effect in the already-playing sweep and `52` had no reported effect in an already-on follow-up. Those negative results remain state-bound evidence, not errors to erase. ROOT source actions `81–86`, power-off `62`, and the `LOCK*` family remain scoped to their noted setup ([sweep and lock tests](notes/kenwood-xs-reverse-engineering-v18.md#L175-L282)).

ROOT `A` cells `08/18/38/48/49/69/74/78/88/C8/F4` preserve older NORMAL labels that differ from controlled ROOT/CD or have clearer effects after `17` ([historical labels](notes/kenwood-xs-reverse-engineering-reviewed.md#L429-L449), [context findings](notes/kenwood-xs-reverse-engineering-v18.md#L537-L562)). `F1` was called STOP in [copy 7](notes/kenwood-xs-reverse-engineering%20copy%207.md#L343-L354) but is a no-visible-effect dot in the later [v18 ROOT matrix](notes/kenwood-xs-reverse-engineering-v18.md#L250-L269). `C3/D3/E3` produced immediate later-decoder RX `D6` without a proven action; delayed `D6` also followed `71/9C` STOP and repeat `B6`, so it is not simply a STOP acknowledgment ([timing comparison](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329)). `32/4C/50` had explicit no-RX and no-visible-effect follow-ups ([follow-up](notes/kenwood-xs-reverse-engineering-v18.md#L270-L276)).

## 2. After logical TX `0x17` matrix

This is a **selective** entered-context map. `S` means tested no RX, even when no physical action was noticed; `U` means the available record does not establish an individual test. `07` is ROOT exit and `09` is volume STOP only in the tested conditions. The `17` cell itself remains `U`: repeated `17` after entry was not separately demonstrated.

| hi\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** | S | U | U | U | R | R! | U | K | R | K | S | S | U | S | S | S |
| **1** | S | U | U | S | R | R! | U | U | R | U | U | S | U | S | S | S |
| **2** | S | U | U | U | R | R! | U | U | U | U | U | S | U | S | S | S |
| **3** | S | U | U | U | R | S | S | U | U | R! | U | S | U | S | S | S |
| **4** | S | U | R | U | R | S | S | U | U | R | U | S | U | S | S | S |
| **5** | S | U | U | U | R | S | S | U | U | U | U | S | U | S | S | S |
| **6** | S | U | U | U | R | S | S | S | U | R | U | S | U | S | S | S |
| **7** | S | U | U | U | R | R! | S | S | U | U | U | S | U | S | S | S |
| **8** | S | U | U | U | R | U | U | U | U | U | R | S | U | S | S | S |
| **9** | S | U | U | U | R | U | U | U | U | U | U | S | U | S | S | S |
| **A** | S | U | U | U | T | S | S | U | U | R | S | S | U | S | S | S |
| **B** | S | U | U | U | R | U | R | U | U | U | U | S | R | S | S | S |
| **C** | S | U | U | U | T | U | U | U | R | U | U | S | R | S | S | S |
| **D** | S | U | U | U | T | S | S | U | R | U | U | S | U | S | S | S |
| **E** | S | U | U | U | R | R | R | U | U | R! | U | S | U | S | S | S |
| **F** | S | U | U | U | R | U | U | U | U | R! | U | S | U | S | S | S |

**Entered-context evidence.** Reported tuner actions: preset `04`, `14–94`; auto tune `05`; preset scan `15`; memory/direct-entry-like `25`; frequency entry `75`; band `B4`; tune `E4/F4` ([legacy tuner table](notes/kenwood-xs-reverse-engineering-v18.md#L380-L417), [logical follow-up](notes/kenwood-xs-reverse-engineering-v18.md#L470-L485)). Reported receiver actions: `08` PHONO; `39` power off with RX `B9`; `49` display segments; `69` CD DIRECT; `A9` SURROUND; `C8` SLEEP; `D8` MUTE; `E9/F9` continuous volume, stopped by `09`, followed by `07` ROOT exit ([targeted controls](notes/kenwood-xs-reverse-engineering-v18.md#L553-L562)). CD actions `18/42/8A/B6/BC/CC/E5/E6` were also reported here, sometimes with an unexpected TUNER input change ([cross-context trials](notes/kenwood-xs-reverse-engineering-v18.md#L486-L498)). `A4/C4/D4` had specifically no visible effect in an older entered-context tuner-family pass ([legacy table](notes/kenwood-xs-reverse-engineering-v18.md#L380-L417)); they are `T`, not `S`.

The 98 `S` cells include five no-RX low-nibble families (`?0/?B/?D/?E/?F`) and selected explicit no-RX tests. They are **not** 98 established NOPs. In particular, isolated `09` with no active volume produced no RX in a later operator report, while `09` after `E9` physically stopped the volume movement in a separate report; the latter is the control observation represented by `K` ([context note](notes/kenwood-xs-reverse-engineering-v18.md#L553-L562)). Neither later trial deposited a raw trace. The lock-family bytes had no ROOT-style lock or RX here, except `75` had a documented different tuner effect ([targeted sweep](notes/kenwood-xs-reverse-engineering-v18.md#L553-L562)).

## 3. Union: command establishment and unknown candidates

`E` = at least one context has a documented physical action, context change, or state-associated response; this **does not establish a unique semantic command name** or safety. `A` = only ambiguous/contradictory action evidence or an unassigned generic RX report remains. `T` = sent in the reported ROOT sweep but no command effect is established in either context; this is a previously tested candidate, **not** a NOP. `U` = neither context documents a test. ROOT's reported full sweep makes `U` count zero here, although its raw per-byte log is absent. The entered-context matrix above retains its 119 distinct `U` cells. `C3/D3/E3` are `A` in this candidate view because RX `D6` alone does not identify their action; `F1` is `A` because STOP conflicts with the later dot. A historically conflicting ROOT cell with a documented action in either context is `E`, while the ROOT matrix still flags the conflict.

| hi\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** | T | T | T | T | E | E | T | E | E | E | T | T | T | T | T | T |
| **1** | T | T | T | E | E | E | T | E | E | T | T | T | T | T | T | T |
| **2** | T | E | T | T | E | E | T | T | E | T | T | T | T | T | T | T |
| **3** | T | E | T | T | E | E | E | T | E | E | T | T | T | T | T | T |
| **4** | T | E | E | T | E | E | E | T | E | E | T | T | T | T | T | T |
| **5** | T | E | E | T | E | E | E | T | E | T | T | T | T | T | T | T |
| **6** | T | E | E | T | E | E | E | E | E | E | E | T | T | T | T | T |
| **7** | T | E | T | T | E | E | E | E | E | T | E | T | T | T | T | T |
| **8** | T | E | E | E | E | E | E | T | E | T | E | T | T | T | T | T |
| **9** | T | T | T | T | E | T | T | T | E | T | T | T | E | T | T | T |
| **A** | T | E | T | T | T | E | E | T | T | E | T | T | T | T | T | T |
| **B** | T | T | T | T | E | T | E | T | T | T | T | T | E | T | T | T |
| **C** | T | T | T | A | T | T | T | T | E | T | T | T | E | T | T | T |
| **D** | T | T | T | A | T | E | E | T | E | T | E | T | T | T | T | T |
| **E** | T | T | T | A | E | E | E | T | T | E | E | T | E | T | T | T |
| **F** | T | A | T | T | E | T | T | T | T | E | T | T | E | T | T | T |

### Coverage accounting and state limits

| View | Physical report `V` | Historical action/RX `R` | Control `K` | No-visible-effect `T` | No-RX-only `S` | Ambiguous `A` | Not documented tested `U` |
|---|---:|---:|---:|---:|---:|---:|---:|
| ROOT | 8 | 46 (21 marked `!`) | 2 (1 marked `!`) | 188 | 0 | 12 | 0 |
| After `17` | 0 | 34 (7 marked `!`) | 2 | 3 | 98 | 0 | 119 |

The union has **82 `E`, 4 `A`, 170 `T`, 0 `U`**. These are documentation counts, not 82 universally valid commands or 170 safe NOPs. The ROOT sweep is a reported one-state visible-effect survey; no per-byte raw sweep file exists. The only retained [serial scan](captures/kenwood-xs-scan.txt#L499-L515) stops at legacy candidate `17`, and its PREP/RECOVER labels must not be treated as proven general release commands. Effects at other power, source, changer, monitor, playback, lock, or entered-context states remain open. The `V` category includes operator reports without raw traces, so it is stronger physical testimony but not independently reproducible from this archive.

### Selected context differences

| Logical TX | ROOT observation | After `17` | Evidence limit |
|---|---|---|---|
| `13` | Source/MONITOR pair, later physically checked | No RX; could restore normal display after `49` | [query note](notes/xs8-readback-2026-10-11.md), [context test](notes/kenwood-xs-reverse-engineering-v18.md#L548-L562) |
| `15` | Releases observed control lock | Tuner preset scan | [lock test](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L206), [tuner test](notes/kenwood-xs-reverse-engineering-v18.md#L470-L485) |
| `75` | Lock effect | Direct frequency entry | [cross-context](notes/kenwood-xs-reverse-engineering-v18.md#L537-L555) |
| `84` | CD input selection | Tuner preset CH8 | [A/B](notes/kenwood-xs-reverse-engineering-v18.md#L410-L428) |
| `8A` | Next disc, CD input retained | Next disc, TUNER input side effect | [direct A/B](notes/kenwood-xs-reverse-engineering-v18.md#L486-L498) |
| `E5/E6` | Current disc restart | Restart plus reported TUNER side effect | [context follow-up](notes/kenwood-xs-reverse-engineering-v18.md#L486-L498) |
| `07` | No visible effect in CD-playing sweep; did not release ROOT lock | Restored ROOT interpretation | [lock negative](notes/kenwood-xs-reverse-engineering-v18.md#L194-L205), [exit test](notes/kenwood-xs-reverse-engineering-v18.md#L558-L562) |
| `09` | No visible effect in CD-playing sweep | STOP after active volume; no RX when isolated without active hold | [targeted controls](notes/kenwood-xs-reverse-engineering-v18.md#L553-L562); later isolated no-RX report has no raw trace |

## Absolute-volume-query findings retained from the earlier coverage review

ROOT `13` and `14` reportedly returned two frames unchanged across manual volume adjustment, and passive monitoring during manual adjustment found no RX. The bounded `17 → E9 → 09 → 07` volume-down trial produced no extra response frames; isolated entered-context `09` was also RX-silent. These were operator reports without deposited edge traces ([historical `14/15` review](notes/xs8-readback-2026-10-11.md#L41-L62), [volume behavior in v18](notes/kenwood-xs-reverse-engineering-v18.md#L553-L562)). The later physically checked `13` fields track source and TAPE2 MONITOR, not an identified absolute-volume value ([October 11 note](notes/xs8-readback-2026-10-11.md#L13-L39)). ROOT `15` can return a status-like pair when unlocked but also releases a lock; its unknown field semantics and side effect prevent calling it a safe volume query ([lock tests](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L206)). ROOT `C3/D3/E3` returned `D6` in changer tests with no established volume relationship ([RX timing](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329)). **No absolute-volume query is established.**

The earlier volume-query priorities remain historical research leads, not validated commands: (1) compare ROOT `15` responses at two modest manually set levels only with its lock-release effect accounted for; (2) retain `13/14` as controls despite their reported unchanged frames; (3) treat `C3/D3/E3` as low-priority changer-response candidates with no volume link; and (4) repeat passive capture during manual volume change as a non-transmitting diagnostic. Any new comparison would need raw edge CSV, decoder validity/gap data, and a recorded physical receiver state. No candidate has yielded an identified absolute-volume field ([query/monitor comparison](notes/xs8-readback-2026-10-11.md#L13-L39), [changer RX comparison](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329)).

## Generic KEY RELEASE hypothesis

**No context-independent KEY RELEASE is established.** A byte can end an observed activity without representing a universal button-release packet. No archived capture isolates XS8 traffic *at the instant of releasing* a physical CD TRACK/SEEK button. The passive changer tests found no XS8 traffic for NEXT/PREV while already playing and documented state/event bytes for PLAY, PAUSE, and STOP; they did not establish a transport key-up byte ([passive button tests](notes/kenwood-xs8-sniffing-notes.md#L49-L103)). Do not reinterpret passive RX as logical TX.

| Rank and candidate | Supporting observation | Alternative interpretation and limit |
|---|---|---|
| 1. ROOT `9C` for CD TRACK/SEEK termination, **not generic** | Older ROOT map explicitly says `9C` stopped continuous forward skip after `FC`; `9C` stopped playback in later trials and the new physical `41 → 9C` trial ended in STOP ([copy 7](notes/kenwood-xs-reverse-engineering%20copy%207.md#L437-L445), [STOP comparison](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329), [new report](notes/xs8-root-validation-2026-10-12.md)). | `9C` may simply stop the CD transport, indirectly ending skip. The new chain did **not** test `FC`, `EC`, `6A`, or `7A`. No general release semantics follows. |
| 2. ROOT `15` for a broader cancel/release family, **weak** | Reliably cleared a persistent control lock; legacy TX `57` was used as PREP/RECOVER in the retained scan and elicited status-like RX ([lock A/B](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L206), [scan](captures/kenwood-xs-scan.txt#L29-L56)). | The effect may be specific to a lock/submode; after `17`, `15` starts tuner preset scan. The scan's “recovery” label was a hypothesis, not proof of generic release. No CD TRACK/SEEK termination test is archived. |
| 3. Entered-context `09` for continuous volume only | `17 → E9/F9 → 09 → 07` stopped volume in the tested setup; isolated `09` produced no RX ([targeted controls](notes/kenwood-xs-reverse-engineering-v18.md#L553-L562), [bounded volume trial](COMMAND_REGISTRY.md#documented-sequences)). | It may be a volume-specific stop token. No ROOT or CD latched-operation release was demonstrated; TX completion or silence cannot prove receiver execution. |
| 4. Context `07` and transport-state commands `41/42`, **low** | `07` exits after `17`; `41` played and `42` paused in their tested CD states ([context exit](notes/kenwood-xs-reverse-engineering-v18.md#L558-L562), [PLAY report](notes/xs8-root-validation-2026-10-12.md), [PAUSE test](notes/kenwood-xs-reverse-engineering-v18.md#L365-L378)). | `07` failed to release the separate ROOT lock; PLAY/PAUSE may merely change playback state. No evidence makes them release packets. |

Other apparent patterns are not enough: ROOT `21/31/51/61/71/A1` stopped CD in one baseline, but `F1` has conflicting STOP/no-effect reports, and the common low nibble does not license new commands. The `C3/D3/E3 → D6` RX family does not prove STOP or RELEASE. After `17`, `15` is a preset scan and `09` is a volume stop, showing why numeric pairings cannot be generalized ([STOP family](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329), [historical `F1`](notes/kenwood-xs-reverse-engineering%20copy%207.md#L343-L354)).

### Ordered research plan (no commands sent here)

1. Preserve the raw REPL/monitor edge CSV and monotonic TX timestamps for the already reported `41 → 9C` trial if available; record changer state and physical button actions. Recheck the older notes for a real *button-release* edge capture rather than assuming an RX state event is release traffic.
2. Before any latched command test, demonstrate an independent, reachable physical changer STOP or power recovery that works in the intended state. A successful `9C` playback STOP trial alone is insufficient proof that the same recovery will work during continuous skip.
3. In a future attended, separately authorized trial, compare one short bounded ROOT `FC → 9C` sequence only after recovery is validated, archiving TX waveform matches, event gaps, changer display/motion, and whether `9C` stops **skipping**, **playback**, or both. Do not use a broad scan or automatic retries. Defer `EC/6A/7A`, ROOT `15`, and context changes until the first mechanism and its recovery are characterized.
4. If testing a claimed generic release later, require the same candidate to terminate more than one independently reproduced activity without changing unrelated receiver state; also test the negative control in an idle state. No present evidence satisfies that criterion.

No new hardware transmission was made for this consolidation. `T`, `S`, and `U` cells must never be treated as safe NOPs. The current files do not independently verify the reported October 12 physical outcomes or a general KEY RELEASE opcode.
