# XS8 command coverage and volume-query candidates

This is an evidence map for **logical TX** bytes `0x00`–`0xFF`, separately in ROOT and after logical TX `0x17`. It is a map of recorded tests, not a command dictionary or a claim of safety. The principal ROOT baseline was **CD selected and playing**; a source change was restored to CD between tests, and `0x15` released the observed control lock when necessary ([v18, lines 210–249](notes/kenwood-xs-reverse-engineering-v18.md#L210-L249)). The `0x17` tests were targeted, with a different and less complete state history ([v18, lines 508–561](notes/kenwood-xs-reverse-engineering-v18.md#L508-L561)).

## Notation and classification

- Every row/column address here is a **current logical TX byte**, `0xHL` at row `H`, column `L`. The older TX notation describes the same waveform with `logical = reverse8(~legacy)`. For example, legacy TX `0x57` is logical TX `0x15`; legacy TX `0x60/0x68` are logical TX `0xF9/0xE9`; legacy TX `0x1F` is logical TX `0x07` ([registry, lines 14–24](COMMAND_REGISTRY.md#L14-L24)). The retained partial serial scan uses **legacy TX labels**, so its candidate `0x07` or `0x09` is not the logical byte shown at those cells here ([scan, lines 29–36](captures/kenwood-xs-scan.txt#L29-L36)).
- `O` = **OBSERVED** physical behavior, context change, or RX; this does **not** imply an RX response. `N` = **NO RESPONSE**, an explicit no-RX test with no separately documented action. `U` = **UNKNOWN** RX status or effect from the available record. Many ROOT `U` cells were sent in the full sweep and had *no visible effect recorded*, but the sweep legend does not establish RX silence for each one ([v18, lines 225–227](notes/kenwood-xs-reverse-engineering-v18.md#L225-L227)). `C` = **CONFLICT**, an earlier, incompletely controlled “NORMAL” attribution that differs materially from the controlled ROOT/CD observation; this flags a documentation conflict, not proof of contradictory behavior in an identical receiver state ([registry, lines 45–48](COMMAND_REGISTRY.md#L45-L48)). `C` takes precedence over `O` or `N` in the display; the underlying controlled observation is given below.
- RX labels below are the **later legacy RX decoder’s printed bytes** and are never run through the TX conversion. Early passive RX source labels differ from later labels, perhaps because of decoder polarity, but the historical cause is unverified ([registry, lines 18–24](COMMAND_REGISTRY.md#L18-L24); [RX sketch, lines 349–355](sketches/xs-tx-test2.ino#L349-L355)). A blank/no-RX result is not evidence of no action or a NOP.
- The new observations supplied with this task (ROOT `0x13/0x14` stable across manual volume adjustment; passive volume adjustment silent; `0x17` `0x09` alone silent; and the bounded volume-down sequence silent beyond its commands) have **no raw traces in this repository**. They are reported bench evidence, not independently auditable archived captures. Reliable capture after the GPIO23/GPIO27 physical connection fix likewise comes from the supplied report, not an archived trace.

## ROOT matrix

Controlled baseline: ROOT, CD selected and playing. `C` historical attribution is overlaid on this baseline. At `0x52`, the controlled follow-up explicitly found no RX and no visible effect despite the old “power on” attribution ([v18, lines 270–272](notes/kenwood-xs-reverse-engineering-v18.md#L270-L272); [registry, line 46](COMMAND_REGISTRY.md#L46)).

| hi\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** | U | U | U | U | U | U | U | U | C | U | U | U | U | U | U | U |
| **1** | U | U | U | O | O | O | U | O | C | U | U | U | U | U | U | U |
| **2** | U | O | U | U | U | U | U | U | O | U | U | U | U | U | U | U |
| **3** | U | O | N | U | U | O | O | U | C | U | U | U | U | U | U | U |
| **4** | U | U | O | U | U | O | O | U | C | C | U | U | N | U | U | U |
| **5** | N | O | C | U | U | O | O | U | O | U | U | U | U | U | U | U |
| **6** | U | O | O | U | U | O | O | O | O | C | O | U | U | U | U | U |
| **7** | U | O | U | U | C | O | O | O | C | U | O | U | U | U | U | U |
| **8** | U | O | O | O | O | O | O | U | C | U | O | U | U | U | U | U |
| **9** | U | U | U | U | U | U | U | U | O | U | U | U | O | U | U | U |
| **A** | U | O | U | U | U | O | O | U | U | U | U | U | U | U | U | U |
| **B** | U | U | U | U | U | U | O | U | U | U | U | U | O | U | U | U |
| **C** | U | U | U | O | U | U | U | U | C | U | U | U | O | U | U | U |
| **D** | U | U | U | O | U | O | O | U | U | U | O | U | U | U | U | U |
| **E** | U | U | U | O | U | O | O | U | U | U | O | U | O | U | U | U |
| **F** | U | U | U | U | C | U | U | U | U | U | U | U | O | U | U | U |

**ROOT evidence by cell/family.** `0x13` returns a source-dependent first RX (`0x11/13/19/1A/1B/1D`) followed roughly 110–120 ms later by monitor-dependent RX (`0x18` off, `0x14` on); these are later-decoder RX values, not TX commands ([reviewed, lines 113–175](notes/kenwood-xs-reverse-engineering-reviewed.md#L113-L175)). `0x14` returned `0x1D,0x18` in ROOT+CD, matching `0x13` in that state; equivalence outside it is unverified ([v18, lines 270–272](notes/kenwood-xs-reverse-engineering-v18.md#L270-L272)). `0x15` released the observed locked-control state and could return `0x1D,0x18` when already unlocked ([reviewed, lines 177–203](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L203); [registry, line 34](COMMAND_REGISTRY.md#L34)). `0x17` changes subsequent interpretation ([v18, lines 537–562](notes/kenwood-xs-reverse-engineering-v18.md#L537-L562)).

The direct CD track family `0x18/28/38/48/58/68/78/88/98` selects tracks 1–9 in the controlled CD state, without implying behavior for other sources ([v18, lines 284–300](notes/kenwood-xs-reverse-engineering-v18.md#L284-L300)). `0x21/31/51/61/71/A1/9C` stopped CD; `0x71/9C` had delayed RX `0xD6`. `0xC3/D3/E3` had immediate RX `0xD6` without a proven STOP meaning, and repeat `0xB6` also gave `0xD6` ([v18, lines 302–329](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329)). `0x42` paused; `0xBC` toggled pause/play with RX `0xDA` on pause or `0xDE`, then `0x7D`, on resume ([v18, lines 365–378](notes/kenwood-xs-reverse-engineering-v18.md#L365-L378)). Passive pause RX `0xD2` remains a distinct unresolved observation ([v18, lines 331–353](notes/kenwood-xs-reverse-engineering-v18.md#L331-L353)).

The full ROOT matrix records the remaining `O` actions: lock-producing `0x35/36/45/46/55/56/65/66/67/75/76/77/A5/A6/D5/D6` ([reviewed, lines 177–203](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L203)); power off `0x62`; tuner/phono/CD/tape selection `0x81`–`0x86`; continuous CD operations `0x6A/7A/EC/FC`; next disc `0x8A`; CCRS display `0xB6`; repeat `0xCC`; CD +10 `0xDA`; restart `0xE5/E6`; time display `0xEA` ([v18, lines 225–282](notes/kenwood-xs-reverse-engineering-v18.md#L225-L282)). `0x81` selected tuner **without immediate RX** and `0xCC` toggled repeat **without RX**, demonstrating why they are `O` rather than `N` ([v18, lines 270–282](notes/kenwood-xs-reverse-engineering-v18.md#L270-L282)). The only ROOT `N` cells not overlaid by `C` are `0x32/4C/50`: explicit no RX and no reported visible effect; `0x52` has the same controlled result but is shown as `C` ([v18, lines 270–272](notes/kenwood-xs-reverse-engineering-v18.md#L270-L272)).

The `C` cells are `0x08/18/38/48/49/52/69/74/78/88/C8/F4`. Earlier “NORMAL” notes attributed actions including source selection, display control, power on, and tuning to these converted bytes, while the later controlled ROOT+CD sweep either found different CD behavior or recorded no visible action. The earlier context and initial state cannot be reconstructed; several of these actions are now documented after `0x17`. Preserve the older reports rather than silently replacing them ([registry, lines 45–48](COMMAND_REGISTRY.md#L45-L48); [history, lines 35–41](RESEARCH_HISTORY.md#L35-L41); [v18 ROOT matrix, lines 251–268](notes/kenwood-xs-reverse-engineering-v18.md#L251-L268)).

## After `0x17` matrix

The byte `0x17` is sent once from ROOT, then the matrix byte is sent while the receiver is in the observed alternative context. `0x07` returns to ROOT in the tested sequence. The map does not establish a formal protocol name for this context ([v18, lines 537–562](notes/kenwood-xs-reverse-engineering-v18.md#L537-L562)).

| hi\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** | N | U | U | U | O | O | U | O | O | O | N | N | U | N | N | N |
| **1** | N | U | U | N | O | O | U | U | O | U | U | N | U | N | N | N |
| **2** | N | U | U | U | O | O | U | U | U | U | U | N | U | N | N | N |
| **3** | N | U | U | U | O | N | N | U | U | O | U | N | U | N | N | N |
| **4** | N | U | O | U | O | N | N | U | U | O | U | N | U | N | N | N |
| **5** | N | U | U | U | O | N | N | U | U | U | U | N | U | N | N | N |
| **6** | N | U | U | U | O | N | N | N | U | O | U | N | U | N | N | N |
| **7** | N | U | U | U | O | O | N | N | U | U | U | N | U | N | N | N |
| **8** | N | U | U | U | O | U | U | U | U | U | O | N | U | N | N | N |
| **9** | N | U | U | U | O | U | U | U | U | U | U | N | U | N | N | N |
| **A** | N | U | U | U | U | N | N | U | U | O | N | N | U | N | N | N |
| **B** | N | U | U | U | O | U | O | U | U | U | U | N | O | N | N | N |
| **C** | N | U | U | U | U | U | U | U | O | U | U | N | O | N | N | N |
| **D** | N | U | U | U | U | N | N | U | O | U | U | N | U | N | N | N |
| **E** | N | U | U | U | O | O | O | U | U | O | U | N | U | N | N | N |
| **F** | N | U | U | U | O | U | U | U | U | O | U | N | U | N | N | N |

**`0x17` evidence by cell/family.** `O` for `0x04/14/24/34/44/54/64/74/84/94` means tuner preset CH10/CH1–9; `0x05` auto tune, `0x15` preset scan, `0x25` memory/direct-entry behavior (precise semantics unresolved), `0x75` direct frequency entry, `0xB4` band, and `0xE4/F4` tune down/up ([reviewed, lines 470–485](notes/kenwood-xs-reverse-engineering-reviewed.md#L470-L485)). `0x08` selects PHONO; `0x39` powers off with RX `0xB9`; `0x49` lights all display segments; `0x69` toggles CD DIRECT; `0xA9` toggles SURROUND; `0xC8` toggles SLEEP; `0xD8` toggles MUTE. `0xE9/F9` start continuous volume down/up and `0x09` stops it. `0x07` returned to ROOT, shown by subsequent `0x13` RX `0x1D,0x18` ([v18, lines 553–562](notes/kenwood-xs-reverse-engineering-v18.md#L553-L562)). The note calls `0x17` `CTX` in this map, but a second `0x17` within the context has no separately demonstrated effect, so its cell remains `U` ([v18, lines 508–535](notes/kenwood-xs-reverse-engineering-v18.md#L508-L535)).

CD functions also observed after `0x17`: `0x18` track 1, `0x42` pause, `0x8A` next disc, `0xB6` CCRS with RX `0xD8`, `0xBC` play/pause with RX `0xDE/DA`, `0xCC` repeat **without RX**, and `0xE5/E6` restart. Some changer actions in this context also switched receiver input to TUNER; causal mechanism is unknown ([reviewed, lines 486–498](notes/kenwood-xs-reverse-engineering-reviewed.md#L486-L498)). The `N` cells cover all tested `0x?0`, `0x?B`, `0x?D`, `0x?E`, `0x?F` families, plus explicit no-RX `0x0A`, `0xAA`, `0x13`, and ROOT lock-family members other than action-bearing `0x75` ([v17, lines 531–537](notes/kenwood-xs-reverse-engineering-v17.md#L531-L537); [v18, lines 548–561](notes/kenwood-xs-reverse-engineering-v18.md#L548-L561)). A no-RX family cell may still have an unrecorded physical effect. The selected other columns were not enumerated byte by byte in the retained notes; they remain `U`, including cells that the summary `·` marked no action in one run ([v18, lines 508–535](notes/kenwood-xs-reverse-engineering-v18.md#L508-L535)). The user-supplied isolated `0x09` test adds that it produced no RX while no volume command was active; its stop action after `0xE9` keeps it `O`.

## Context comparisons and ROOT RX of uncertain meaning

| Logical TX | ROOT | After `0x17` | Evidence / limit |
|---|---|---|---|
| `0x13` | Two source/monitor-like RX frames; user reports pair unchanged after a manual volume adjustment | No RX; `0x13` can restore the display after `0x49` despite RX silence | [reviewed, lines 113–175](notes/kenwood-xs-reverse-engineering-reviewed.md#L113-L175); [v18, lines 548–558](notes/kenwood-xs-reverse-engineering-v18.md#L548-L558); new user observation lacks archived trace |
| `0x14` | Same `0x1D,0x18` pair as `0x13` in CD state; user reports two frames unchanged after manual volume adjustment | Preset CH1 | [v18, lines 270–272](notes/kenwood-xs-reverse-engineering-v18.md#L270-L272); [reviewed, lines 474–485](notes/kenwood-xs-reverse-engineering-reviewed.md#L474-L485); new user observation lacks archived trace |
| `0x15` | Releases control lock and may elicit status-like RX when already unlocked | Preset scan | [reviewed, lines 177–203](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L203); [v18, lines 537–551](notes/kenwood-xs-reverse-engineering-v18.md#L537-L551) |
| `0x75` | Locks controls in tested CD state | Direct-frequency entry, no ROOT-style lock or RX in targeted test | [reviewed, lines 177–203](notes/kenwood-xs-reverse-engineering-reviewed.md#L177-L203); [v18, lines 548–555](notes/kenwood-xs-reverse-engineering-v18.md#L548-L555) |
| `0x84` | Selects CD with RX `0x7D` | Tuner preset CH8 | [v18, lines 270–276](notes/kenwood-xs-reverse-engineering-v18.md#L270-L276); [v18, lines 537–551](notes/kenwood-xs-reverse-engineering-v18.md#L537-L551) |
| `0x8A` | Next disc, CD input remains selected | Next disc, TUNER input side effect | [reviewed, lines 486–498](notes/kenwood-xs-reverse-engineering-reviewed.md#L486-L498) |
| `0xE5/0xE6` | Restart current CD | Restart; TUNER input side effect | [v18, lines 537–547](notes/kenwood-xs-reverse-engineering-v18.md#L537-L547) |

ROOT commands with RX whose *response semantics are unresolved* include `0xC3/0xD3/0xE3` (immediate `0xD6`), `0x71/0x9C` (delayed `0xD6` despite observed CD stop), and repeat `0xB6` (`0xD6` while CCRS stays active). These are poor evidence for volume because their tested setting involved the CD changer, and the same RX value arose from different actions ([v18, lines 302–363](notes/kenwood-xs-reverse-engineering-v18.md#L302-L363)). `0x15` can elicit the source/monitor-like `0x1D,0x18` pair when unlocked; its higher-level purpose remains unresolved ([registry, line 34](COMMAND_REGISTRY.md#L34)). RX `0x35` after `0xEC` was observed once and is not assigned a meaning ([v18, lines 331–342](notes/kenwood-xs-reverse-engineering-v18.md#L331-L342)). No archived command produced a frame identified as absolute volume.

## Coverage gaps and candidate experiments

ROOT has a full **visible-effect sweep** in one CD-playing baseline, but no exhaustive byte-by-byte RX record in the archive. Its `U` cells are not untested in that baseline; their RX and other-state effects are unestablished. The `0x17` map has five fully probed no-RX low-nibble families and targeted selections elsewhere. The original targeted per-byte logs are absent, so `U` does not always mean never transmitted. The retained serial scan is a **different, older legacy-TX experiment** that stops at candidate legacy `0x17`; it cannot substantiate either full matrix ([scan, lines 499–515](captures/kenwood-xs-scan.txt#L499-L515); [history, lines 44–50](RESEARCH_HISTORY.md#L44-L50)). The recent RX findings and sniffer connection fix were reported by the user without new CSV evidence. State dependence outside KR-V6070, connected changer, CD playback, source, monitor, power, and lock conditions is unknown.

**No positive absolute-volume-query candidate is established.** Ranked next experiments, all requiring an operator and controlled receiver state:

1. **ROOT `0x15` — weak best command candidate.** It is an observed RX producer even when already unlocked, yet the archived record does not say whether that pair changes with absolute volume. Compare full raw RX frames at two manually chosen, modest levels, returning to a known unlocked ROOT state. Its known lock-release effect makes it unsuitable as a transparent query until tested ([registry, line 34](COMMAND_REGISTRY.md#L34)).
2. **ROOT `0x13` and `0x14` — controls, currently disfavored as simple volume queries.** Both reportedly returned the same two frames before and after manual volume adjustment. Repeat with archived edge CSV and a logged visible volume reading, same source and monitor state; include an RX source/monitor check to validate the decoder. The new negative observation is useful but does not exclude an unobserved third frame, another state, or an encoded timing property ([reviewed, lines 113–175](notes/kenwood-xs-reverse-engineering-reviewed.md#L113-L175); user-supplied observation).
3. **ROOT `0xC3/0xD3/0xE3` — low-priority RX candidates with changer risk.** Immediate `0xD6` is well documented, but no evidence ties it to volume. Only compare responses after a non-transmitting reanalysis of captured traces and with the changer state controlled; avoid interpreting `0xD6` as a volume value ([v18, lines 302–329](notes/kenwood-xs-reverse-engineering-v18.md#L302-L329)).
4. **Passive capture during a controlled manual volume change — diagnostic control, not a TX candidate.** The new reliable GPIO23/GPIO27 sniffer and the user-reported silent passive test make unsolicited volume broadcasts unlikely in that state. Retain raw BUSY/DATA edge CSV before further query trials. This does not prove the interface has no absolute-volume report in other conditions (user-supplied observation; no archived trace).

Do not choose candidates by neighboring byte values or numerical patterns. Many `0x17` `U` cells also have unknown ROOT behavior: for example `0x01/02/03`, `0x11/12`, `0x22/23`, `0x33`, `0x41/43`, and many other positions in the selectively tested columns. These are **coverage gaps, not proposed commands**. Conversely, `0x17` `0x14/15/75/84` have known behavior despite very different or uncertain ROOT meanings; probe context explicitly and verify the `0x07` exit with `0x13` before interpreting a result ([v18, lines 537–562](notes/kenwood-xs-reverse-engineering-v18.md#L537-L562)).

## Safety boundaries

Known ROOT hazards include power off `0x62`, lock-producing `0x35/36/45/46/55/56/65/66/67/75/76/77/A5/A6/D5/D6`, source changes `0x81`–`0x86`, and continuous CD operations `0x6A/7A/EC/FC` ([v18, lines 225–282](notes/kenwood-xs-reverse-engineering-v18.md#L225-L282)). Known `0x17` hazards include power off `0x39`, continuous volume `0xE9/F9` requiring `0x09` STOP before `0x07` ROOT exit, mute `0xD8`, tuner operations, and CD actions with unexpected TUNER input selection ([v18, lines 537–562](notes/kenwood-xs-reverse-engineering-v18.md#L537-L562)). The newly reported `0x17 → 0xE9 → 0x09 → 0x07` trial decreased and stopped volume with no additional RX; this is evidence against a routine volume-status response to STOP in that tested state, not evidence that STOP is universally silent. Do not perform blind sweeps or treat `N`/`U` as safe NOPs. The documented effects apply to the experimental KR-V6070 setup and tested states only.

## Coverage totals

| Context | OBSERVED (`O`) | NO RESPONSE (`N`) | UNKNOWN (`U`) | CONFLICT (`C`) | Total |
|---|---:|---:|---:|---:|---:|
| ROOT | 54 | 3 | 187 | 12 | 256 |
| After `0x17` | 36 | 98 | 122 | 0 | 256 |

`C` overlays 5 otherwise `O` cells, 1 otherwise `N` cell, and 6 otherwise `U` cells. Therefore the controlled ROOT baseline has 59 documented-effect cells, four explicitly no-RX follow-ups, and 193 cells whose RX status is insufficiently recorded. No `0x17` cell meets the `C` criterion in a demonstrated identical context; different actions *between* contexts are expected and kept separate.
