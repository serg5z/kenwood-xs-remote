# XS8 logical-TX command coverage and KEY RELEASE research

This is a consolidated **logical TX** evidence map for the KR-V6070, not a list of safe commands. The ROOT reference is the reported 2026-10-09 full `00–FF` sweep with CD selected and playing; its per-byte raw log was not archived ([v18 baseline and matrix](notes/kenwood-xs-reverse-engineering-v18.md#L210-L276)). The entered-context reference is a selective set of tests after logical TX `0x17`, not a full sweep ([v18 context map](notes/kenwood-xs-reverse-engineering-v18.md#L508-L562)). New physical results are [ROOT power/PLAY/STOP](notes/xs8-root-validation-2026-10-12.md) and [CD KEY RELEASE](notes/xs8-cd-key-release-2026-10-12.md), reported without deposited raw edges or service replies. The corrected v18 ROOT cells for `0x41` and `0x52` and the earlier no-effect observation for `0x72` are preserved as historical state-bound evidence.

## Notation and evidence levels

Each row/column is logical TX `0xHL` (high nibble row, low nibble column). Legacy TX is another label for the **same waveform**, converted by `logical = reverse8(~legacy)`: `B5 → 52`, `7D → 41`, `C6 → 9C`, `C0/C8/A9/A1/B1 → FC/EC/6A/7A/72`, `57 → 15`, `1F → 07`, `60/68 → F9/E9`. Never apply that conversion to RX. Later-decoder legacy RX labels such as `D6`, `1D`, and `18` remain as printed; early passive RX labels require their own convention ([registry encoding history](COMMAND_REGISTRY.md#byte-encoding-and-decoder-history), [original TX routine](sketches/xs-tx-test1.ino#L180-L235), [later RX routine](sketches/xs-tx-test2.ino#L349-L355)). The retained [partial scan](captures/kenwood-xs-scan.txt#L29-L56) uses legacy **TX** labels and ends at candidate legacy `17`, so its candidate labels must be converted before matrix placement.

- `V` — operator-reported **physical verification** in a controlled state. It is not independently replayable without raw trial records. ROOT `13` has the [October 11 readback report](notes/xs8-readback-2026-10-11.md); `41/52/9C` have the [October 12 report](notes/xs8-root-validation-2026-10-12.md); `82–85` were physically reported in the [earlier four-input validation](notes/xs8-root-validation-2026-10-12.md), but its per-trial log is not in this archive. ROOT `62`, `8A`, `A1`, `CC`, `DA` and `EA` and the entered-context `48/78/88` are described in [section 4](#4-physical-validations-reported-2026-10-12-to-2026-10-15). `V` for `62`, `8A`, `A1`, `CC`, `DA`, `EA` and `48` rests on operator statements alone, with no dated trial note. `V` does not mean universal safety or receiver acknowledgment.
- `R` — behavior or RX **reported in historical notes**, without a retained per-byte raw log. The presence of RX can be the only observed result; semantics may remain unknown.
- `K` — **known protocol/control byte** in the tested context: ROOT `15` releases a control lock and ROOT `17` enters an alternative interpretation; after `17`, `07` exits and `09` stops an active volume hold. These are observed control effects, not claims of a universal release opcode.
- `T` — **tested with no visible effect recorded** in the specified state. It does not mean NOP or no RX unless a separate no-RX test says so.
- `S` — **explicitly tested without RX**, but physical effects were not established by that negative result. This is used in the selective context map so no-RX is never silently equated with no action.
- `A` — **ambiguous or conflicting evidence**. See the byte-specific notes below; a different state may explain a discrepancy.
- `U` — **not documented as individually tested** in that context. A summary dot from a selective map is insufficient to turn it into `T`.
- `!` suffix — **known potential side effect or latched behavior**: power-off, control lock, context entry, continuous CD operation, tuner submode/scan, or continuous volume. It marks a conservative hazard, not an exhaustive safety classification. A cell such as `R!` retains its evidence level.

## 1. ROOT matrix

Baseline is ROOT + CD selected + CD playing, with source and lock state restored between trials. `V` for `41`, `52`, `62`, `72`, `8A`, `A1`, `CC`, `DA` and `EA` overlays newer physical tests in different initial states; their earlier no-visible-effect observations remain recorded below. `A` preserves historical NORMAL attributions and the `F1` STOP discrepancy rather than selecting a global meaning.

| hi\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** | T | T | T | T | T | T | T | T | A | T | T | T | T | T | T | T |
| **1** | T | T | T | V | R | K | T | K! | A | T | T | T | T | T | T | T |
| **2** | T | R | T | T | T | T | T | T | R | T | T | T | T | T | T | T |
| **3** | T | R | T | T | T | R! | R! | T | A | T | T | T | T | T | T | T |
| **4** | T | V | R | T | T | R! | R! | T | A | A | T | T | T | T | T | T |
| **5** | T | R | V | T | T | R! | R! | T | R | T | T | T | T | T | T | T |
| **6** | T | R | V! | T | T | R! | R! | R! | R | A | R! | T | T | T | T | T |
| **7** | T | R | V | T | A | R! | R! | R! | A | T | R! | T | T | T | T | T |
| **8** | T | R | V | V | V | V | R | T | A | T | V | T | T | T | T | T |
| **9** | T | T | T | T | T | T | T | T | R | T | T | T | V | T | T | T |
| **A** | T | V | T | T | T | R! | R! | T | T | T | T | T | T | T | T | T |
| **B** | T | T | T | T | T | T | R | T | T | T | T | T | R | T | T | T |
| **C** | T | T | T | R | T | T | T | T | A | T | T | T | V | T | T | T |
| **D** | T | T | T | R | T | R! | R! | T | T | T | V | T | T | T | T | T |
| **E** | T | T | T | R | T | R | R | T | T | T | V | T | R! | T | T | T |
| **F** | T | A | T | T | A | T | T | T | T | T | T | T | R! | T | T | T |

**ROOT evidence.** `13` produced two source/MONITOR-associated RX frames; the six sources and both MONITOR states were later physically compared ([query A/B](notes/kenwood-xs-reverse-engineering-reviewed.md#L113-L175), [physical readback](notes/xs8-readback-2026-10-11.md)). `14` produced the same `1D,18` pair in ROOT+CD but is not proven equivalent elsewhere. `15` released the persistent observed lock; `17` changed interpretation ([lock and context tests](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L206), [context A/B](notes/kenwood-xs-reverse-engineering-reviewed.md#L410-L428)).

The reported CD actions include direct tracks `18/28/38/48/58/68/78/88/98`, STOP `21/31/51/61/71/A1/9C`, PAUSE `42`, PLAY/PAUSE toggle `BC`, next disc `8A`, continuous `6A/7A/EC/FC`, repeat `CC`, CCRS `B6`, restart `E5/E6`, and time display `EA` ([ROOT sweep details](notes/kenwood-xs-reverse-engineering-v18.md#L270-L378)). The new trial physically observed `41` PLAY and `41 → 9C` STOP; `52` powered on a standby receiver while its CD changer powered through the switched AC outlet ([physical report](notes/xs8-root-validation-2026-10-12.md)). October 12 paired trials then physically observed `72` releasing active `FC/EC/6A/7A` CD operations, while `FC → 9C` stopped playback as well as skipping ([CD release report](notes/xs8-cd-key-release-2026-10-12.md)). `41` had no reported effect in the already-playing sweep, `52` had no reported effect in an already-on follow-up, and `72` had no reported effect without an active TRACK/SEEK operation. Those negative results remain state-bound evidence, not errors to erase. ROOT source actions `81–86`, power-off `62`, and the `LOCK*` family remain scoped to their noted setup ([sweep and lock tests](notes/kenwood-xs-reverse-engineering-v18.md#L175-L282)).

ROOT `A` cells `08/18/38/48/49/69/74/78/88/C8/F4` preserve older NORMAL labels that differ from controlled ROOT/CD or have clearer effects after `17` ([historical labels](notes/kenwood-xs-reverse-engineering-reviewed.md#L429-L449), [context findings](notes/kenwood-xs-reverse-engineering-v18.md#L537-L562)). `F1` was called STOP in [copy 7](notes/kenwood-xs-reverse-engineering%20copy%207.md#L343-L354) but is a no-visible-effect dot in the later [v18 ROOT matrix](notes/kenwood-xs-reverse-engineering-v18.md#L250-L269). `C3/D3/E3` produced immediate later-decoder RX `D6` without a proven action; delayed `D6` also followed `71/9C` STOP and repeat `B6`, so it is not simply a STOP acknowledgment ([timing comparison](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329)). `32/4C/50` had explicit no-RX and no-visible-effect follow-ups ([follow-up](notes/kenwood-xs-reverse-engineering-v18.md#L270-L276)).

## 2. After logical TX `0x17` matrix

This is a **selective** entered-context map. `S` means tested no RX, even when no physical action was noticed; `U` means the available record does not establish an individual test. `07` is ROOT exit and `09` is volume STOP only in the tested conditions. `V` at `48`, `78` and `88` marks the physically observed CONTEXT keys TAPE2 MONITOR, VIDEO1 and VIDEO2, each sent as `17 → key → 07` ([section 4](#4-physical-validations-reported-2026-10-12-to-2026-10-15)). The `17` cell itself remains `U`: repeated `17` after entry was not separately demonstrated.

| hi\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** | S | U | U | U | R | R! | U | K | R | K | S | S | U | S | S | S |
| **1** | S | U | U | S | R | R! | U | U | R | U | U | S | U | S | S | S |
| **2** | S | U | U | U | R | R! | U | U | U | U | U | S | U | S | S | S |
| **3** | S | U | U | U | R | S | S | U | U | R! | U | S | U | S | S | S |
| **4** | S | U | R | U | R | S | S | U | V | R | U | S | U | S | S | S |
| **5** | S | U | U | U | R | S | S | U | U | U | U | S | U | S | S | S |
| **6** | S | U | U | U | R | S | S | S | U | R | U | S | U | S | S | S |
| **7** | S | U | U | U | R | R! | S | S | V | U | U | S | U | S | S | S |
| **8** | S | U | U | U | R | U | U | U | V | U | R | S | U | S | S | S |
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
| **7** | T | E | E | T | E | E | E | E | E | T | E | T | T | T | T | T |
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
| ROOT | 9 | 46 (21 marked `!`) | 2 (1 marked `!`) | 187 | 0 | 12 | 0 |
| After `17` | 0 | 34 (7 marked `!`) | 2 | 3 | 98 | 0 | 119 |

The union has **83 `E`, 4 `A`, 169 `T`, 0 `U`**. These are documentation counts, not 83 universally valid commands or 169 safe NOPs. The ROOT sweep is a reported one-state visible-effect survey; no per-byte raw sweep file exists. The only retained [serial scan](captures/kenwood-xs-scan.txt#L499-L515) stops at legacy candidate `17`, and its PREP/RECOVER labels must not be treated as proven general release commands. Effects at other power, source, changer, monitor, playback, lock, or entered-context states remain open. The `V` category includes operator reports without raw traces, so it is stronger physical testimony but not independently reproducible from this archive. `72` is now `E` because of a later active-transport physical test, while its earlier idle-sweep no-effect result remains historical evidence.

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

## 4. Physical validations reported 2026-10-12 to 2026-10-15

These rows record **operator-reported physical observations** made through the production XS8 Service, the Web Gateway or the attended research session. Two evidence levels are distinguished:

- **Dated note:** a trial record is archived in `research/notes/`. It is still operator-written; raw edges and complete service replies were not deposited.
- **Operator statement only:** the operator stated the result while the work was in progress. No trial note, raw capture or complete reply log is archived.

No row rests on `TX_OK`/`MATCH` alone: every one is a physical observation of the receiver or changer. `TX_OK`/`MATCH` never establishes receiver execution. The historical sweep results for the same bytes remain valid for their own states.

| Logical TX | Context / path | Physical observation | Evidence level |
|---|---|---|---|
| `52` | ROOT; receiver in standby; `POWER_ON` | Receiver powered on; the changer powered through the switched outlet | Dated note: [2026-10-12](notes/xs8-root-validation-2026-10-12.md) |
| `62` | ROOT; receiver on; `POWER_OFF` | Receiver powered off | Operator statement only, 2026-10-14 |
| `62` / `52` via the Gateway's one-click toggle | ROOT `13` SETTLE decides: a reply sends `62`; two clean silences ≥ 1 s apart send `52` | Toggled the receiver off and on, both directions | Operator statement only, 2026-10-15 |
| `13` | ROOT; plain and SETTLE query | Two replies: first RX `11/13/19/1A/1B/1D` gives the source, second RX `18`/`14` gives MONITOR OFF/ON (waveform `E7`/`D7`). Silence in standby | Dated note for all six sources and both MONITOR states: [2026-10-11](notes/xs8-readback-2026-10-11.md). The silence-in-standby observation is an operator statement only (2026-10-14) |
| `84 82 83 85` (ROOT) and `17 → 78/88 → 07` (CONTEXT), each followed by a SETTLE `13` | Web source selection with automatic readback | The selected input showed on the receiver, and the readback reported it | VIDEO1/VIDEO2 sequences: dated note, [2026-10-12](notes/xs8-context-video-mute-2026-10-12.md). The end-to-end Web selection with SETTLE readback: operator statement only, 2026-10-15 |
| `17 → 48 → 07` | CONTEXT; `TAPE2_MONITOR_TOGGLE` | Toggled TAPE2 MONITOR. The readback showed `D7` (ON) and `E7` (OFF) in waveform terms, legacy RX `14`/`18` | Operator statement only, 2026-10-15 |
| `A1` | ROOT; CD playing; `CD_STOP` | CD stopped; ROOT `21` also stopped CD via the REPL | Operator statement only, recorded in commit `2793883` (2026-10-14). It replaced `9C`, after a VIDEO1/VIDEO2 selection following a `9C` STOP provoked an unwanted changer restart |
| `EA` | ROOT, CD selected; now `CD_TIME_DISPLAY` | The changer's time display mode changed | Operator statement only, 2026-10-15 |
| `CC` ×2 | ROOT, CD selected; now `CD_REPEAT` | REPEAT indicator on, then off | Operator statement only, 2026-10-15. Earlier partial maps (copies 2–7) recorded `CC` as no visible effect |
| `8A` | ROOT, CD selected, changer connected; now `CD_DISC_SKIP` | Next disc; receiver stayed on CD; a passive `D4` about 1.15 s later | Operator statement only, 2026-10-15. `D4` is a candidate disc-change notification, not a confirmed command |
| `DA → 38`; `DA → DA → 38` (200 ms gaps) | ROOT, CD playing; now `CD_TRACK_SELECT` | Track 13 selected; 23 displayed (no such track on the disc, playback unchanged) | Operator statement only, 2026-10-15; research-chain TX records in the service journal |
| `DA → 08`; `DA` → MUTE → `38` | ROOT, CD playing; MUTE via the service's named CONTEXT command | Track 10 selected (digit 0 confirmed after +10); track 13 selected with MUTE in between (pending +10 survived) | Operator statement only, 2026-10-15; no capture archived. The ROOT `08` cell stays `A`: standalone `08` has no recorded action |

The entered-context `48` result supersedes, **for CONTEXT only**, the open question of the historical NORMAL label `ED` (logical `48`) "MONITOR toggle". In ROOT, `48` remains CD track 4 in the controlled sweep.

## Absolute-volume-query findings retained from the earlier coverage review

ROOT `13` and `14` reportedly returned two frames unchanged across manual volume adjustment, and passive monitoring during manual adjustment found no RX. The bounded `17 → E9 → 09 → 07` volume-down trial produced no extra response frames; isolated entered-context `09` was also RX-silent. These were operator reports without deposited edge traces ([historical `14/15` review](notes/xs8-readback-2026-10-11.md#L41-L62), [volume behavior in v18](notes/kenwood-xs-reverse-engineering-v18.md#L553-L562)). The later physically checked `13` fields track source and TAPE2 MONITOR, not an identified absolute-volume value ([October 11 note](notes/xs8-readback-2026-10-11.md#L13-L39)). ROOT `15` can return a status-like pair when unlocked but also releases a lock; its unknown field semantics and side effect prevent calling it a safe volume query ([lock tests](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L206)). ROOT `C3/D3/E3` returned `D6` in changer tests with no established volume relationship ([RX timing](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329)). **No absolute-volume query is established.**

The earlier volume-query priorities remain historical research leads, not validated commands: (1) compare ROOT `15` responses at two modest manually set levels only with its lock-release effect accounted for; (2) retain `13/14` as controls despite their reported unchanged frames; (3) treat `C3/D3/E3` as low-priority changer-response candidates with no volume link; and (4) repeat passive capture during manual volume change as a non-transmitting diagnostic. Any new comparison would need raw edge CSV, decoder validity/gap data, and a recorded physical receiver state. No candidate has yielded an identified absolute-volume field ([query/monitor comparison](notes/xs8-readback-2026-10-11.md#L13-L39), [changer RX comparison](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329)).

## Generic KEY RELEASE hypothesis

**ROOT `0x72` (legacy TX `0xB1`) is physically validated as a common CD transport KEY UP/RELEASE for `0xFC`, `0xEC`, `0x6A`, and `0x7A` in the reported October 12 trials. A context-independent or device-independent XS8 KEY RELEASE remains unproved.** The earlier ROOT+CD+playing sweep saw no effect from `0x72` because no TRACK/SEEK operation was active; preserve that state-bound result. The passive changer tests did not isolate a physical CD button-up frame, and passive RX must not be reinterpreted as logical TX ([CD release report](notes/xs8-cd-key-release-2026-10-12.md), [historical sweep](notes/kenwood-xs-reverse-engineering-v18.md#L250-L269), [passive tests](notes/kenwood-xs8-sniffing-notes.md#L49-L103)).

| Rank and candidate | Supporting observation | Alternative interpretation and limit |
|---|---|---|
| 1. ROOT `72` for CD transport KEY UP, **physically validated in scope** | `FC → 72`, `EC → 72` (including repeated pairs), `6A → 72`, and `7A → 72` physically ended the corresponding TRACK/SEEK actions in ROOT; trials `id=34–37` identify the later EC/SEEK experiments ([October 12 report](notes/xs8-cd-key-release-2026-10-12.md)). | No evidence establishes `72` as a universal XS8 release, an entered-context release, or a release for other operations. Original per-frame logs are not archived. |
| 2. ROOT `9C` for CD STOP/CANCEL, **not equivalent to KEY UP** | The new `FC → 9C` trial cancelled continuing skip **and stopped playback**; `41 → 9C` previously ended in STOP ([October 12 CD trials](notes/xs8-cd-key-release-2026-10-12.md), [earlier PLAY-to-STOP](notes/xs8-root-validation-2026-10-12.md)). | Stopping the transport can indirectly terminate skipping. `9C` is not demonstrated to leave playback running as a key release would. |
| 3. ROOT `15` for front-panel lock release | Cleared the persistent control lock; legacy TX `57` was used as PREP/RECOVER in the retained scan ([lock A/B](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L206), [scan](captures/kenwood-xs-scan.txt#L29-L56)). | After `17`, `15` starts tuner preset scan. No CD TRACK/SEEK release result is attributed to it. |
| 4. Entered-context `09` for volume STOP; `07` for context exit | `17 → E9/F9 → 09 → 07` stopped volume and returned to ROOT in the tested setup ([targeted controls](notes/kenwood-xs-reverse-engineering-v18.md#L553-L562)). | Their observed roles differ from CD transport `72`. TX completion or RX silence alone cannot prove receiver execution. |

Other apparent patterns are not enough: ROOT `21/31/51/61/71/A1` stopped CD in one baseline, but `F1` has conflicting STOP/no-effect reports, and the common low nibble does not license new commands. The `C3/D3/E3 → D6` RX family does not prove STOP or RELEASE. After `17`, `15` is a preset scan and `09` is a volume stop, showing why numeric pairings cannot be generalized ([STOP family](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329), [historical `F1`](notes/kenwood-xs-reverse-engineering%20copy%207.md#L343-L354)).

### Remaining research limits (no commands sent for this update)

The successful pairs requested 100 ms TX-end-to-next-TX-start gaps; actual gaps were reported as approximately 104–109 ms. `FC → 72` advanced one track near that setting but two near a 500 ms gap. This is no guaranteed one-track timing threshold. Preserve the underlying REPL TX records and raw monitor edges if available, including validity/gaps, before drawing electrical timing conclusions. Future claims of a generic release require tests in other independently reproduced activities and contexts, with physical results separated from TX/waveform evidence. The [candidate study](notes/xs8-key-release-candidates.md) now removes `72` from its unknown list; its pre-discovery rationale is preserved as history.

No new hardware transmission was made for this documentation update. `T`, `S`, and `U` cells must never be treated as safe NOPs. The reported physical results establish CD transport release in the tested ROOT setup, but the archived material cannot independently verify the per-trial electrical records or a general XS8 KEY RELEASE opcode.
