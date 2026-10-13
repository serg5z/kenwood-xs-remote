# Kenwood KR-V6070 XS8 command matrices: ROOT and CONTEXT

Compiled 2026-10-13 from the repository's research record. **No hardware transmission was made for this report.** It brings the existing evidence together; it is not a new test, a safe-command list or a protocol definition. Where it departs from an earlier summary, the departure is stated in [§E](#e-conflicts-uncertain-cells-and-departures-from-earlier-summaries).

## Byte notation: `waveform_lsb` only

Every byte in the matrices and inventory is **`waveform_lsb`**, the field the GPIO decoder prints ([`xs8_decode.c`](../xs8_decode.c#L84-L95)):

- LSB first;
- short DATA LOW = `0`, long DATA LOW = `1`.

For a transmitted frame this is the same as the "current logical TX" value used throughout the research notes, so ROOT `0x84` is the byte the service sends for CD.

The decoder's second field, `legacy_msb_short1` (MSB first, short LOW = `1`), is another label for the **same waveform**: `waveform_lsb = reverse8(~legacy_msb_short1)`. It is not shown in the matrices.

**RX bytes.** Status and response bytes recorded by the later Arduino RX decoder were printed in `legacy_msb_short1` notation; [`xs-tx-test2.ino`](../research/sketches/xs-tx-test2.ino#L349-L355) uses that convention. For example:
- query response `1D` is shown here as `0x47`;
- MONITOR OFF `18` is shown here as `0xE7`.

These conversions are the same derivation the [October 11 readback note](../research/notes/xs8-readback-2026-10-11.md) applied to the query responses. Early passive-sniffer labels from 2026-10-07 are **not** converted or placed, because their decoder relationship is unresolved (see [§D.3](#d3-early-passive-sniffer-labels-not-placed)).

## Cell notation

| Cell | Meaning |
|---|---|
| `NAME` | Known command. The mnemonic names the **observed effect** in the tested state, not proven protocol semantics. |
| `NAME*` | Effect-named state, not command semantics: `LOCK*` sets an observed front-panel lock and `UNLOCK*` releases it (the notes call it `REL*`). |
| `NAME?` | A documented action with **conflicting or uncertain** evidence. See §E. |
| `S` | Known status or response byte, received on the bus in that context's tests, with no TX command known at this cell. |
| `·` | Explicitly tested: no visible reaction and no confirmed effect in the recorded test. This is **not** a NOP: other states remain untested. |
| `?` | Not tested, not documented as individually tested, or evidence insufficient to name an action. |
| `!` | Known hazardous or stateful behavior without a short name. **Unused:** every such byte below has a mnemonic. Hazards are listed in the inventory. |

**Precedence when a byte has several roles:** command > `S` > `·` > `?`. Ten bytes are both a TX command and a known RX value. They are listed in [§D.2](#d2-bytes-that-are-both-a-tx-command-and-a-known-rx-value), and every `S` cell's TX test status is listed in [§D.1](#d1-placed-s-cells).

### Evidence levels used in the inventory

| Level | Meaning |
|---|---|
| **P** | **Physically validated, recent**: operator-observed receiver or changer behavior from attended service/REPL trials on 2026-10-11 or 2026-10-12. No raw edge traces are archived. |
| **H** | **Historical bench observation**: written in the 2026-10-09 (or earlier) notes; no per-byte raw log is archived. |
| **E** | **Electrical only**: `TX_OK` / waveform `MATCH`. This never proves receiver execution. |
| **I** | **Inferred**: interpretation beyond the observation (for example `source_inferred=TV1` from a ROOT query). |

A state-dependent byte can have both a positive result and an older negative result; both are kept.

## A. ROOT matrix

The baseline is the 2026-10-09 full `00–FF` sweep in **ROOT, CD selected, CD playing**, with the source and lock state restored between trials ([v18 sweep](../research/notes/kenwood-xs-reverse-engineering-v18.md#L210-L282)). Later physical results for `41`, `52`, `72`, `9C` and `82–85` are overlaid. Rows are the high nibble and columns the low nibble.

```text
        0        1        2        3        4        5        6        7        8        9        A        B        C        D        E        F
   +------------------------------------------------------------------------------------------------------------------------------------------------
 0 |    ·        ·        ·        ·        ·        ·        ·        ·        ·        ·        ·        ·        ·        ·        ·        ·
 1 |    ·        ·        ·      QUERY     STAT   UNLOCK*     ·       CTX     TRACK1     ·        ·        ·        ·        ·        ·        ·
 2 |    ·       STOP      ·        ·        ·        ·        ·        S      TRACK2     ·        ·        ·        ·        ·        ·        ·
 3 |    ·       STOP      ·        ·        ·      LOCK*    LOCK*      S      TRACK3     ·        ·        ·        ·        ·        ·        ·
 4 |    ·       PLAY    PAUSE      ·        ·      LOCK*    LOCK*      S      TRACK4     ·        ·        ·        ·        ·        ·        ·
 5 |    ·       STOP    PWR-ON     S        ·      LOCK*    LOCK*      ·      TRACK5     ·        ·        ·        ·        ·        ·        ·
 6 |    ·       STOP   PWR-OFF     ·        ·      LOCK*    LOCK*    LOCK*    TRACK6     ·      SEEK-      ·        ·        ·        ·        ·
 7 |    ·       STOP    KEYUP      ·        ·      LOCK*    LOCK*    LOCK*    TRACK7     ·      SEEK+      ·        ·        ·        ·        ·
 8 |    ·      TUNER    TUNER    PHONO      CD     TAPE1    TAPE1      ·      TRACK8     ·      DISC+      ·        ·        ·        ·        ·
 9 |    ·        ·        ·        ·        S        ·        ·        ·      TRACK9     ·        ·        ·       STOP      ·        ·        ·
 A |    ·       STOP      ·        ·        S      LOCK*    LOCK*      S        ·        ·        ·        ·        ·        ·        ·        ·
 B |    ·        ·        ·        ·        ·        ·       CCRS      ·        ·        ·        ·        ·     PLAY/PS     ·        ·        ·
 C |    ·        ·        ·        ?        ·        ·        ·        ·        ·        ·        ·        ·      REPEAT     ·        ·        ·
 D |    ·        ·        ·        ?        ·      LOCK*    LOCK*      S        ·        ·       +10       ·        ·        ·        ·        ·
 E |    ·        ·        ·        ?        S     RESTART  RESTART     S        ·        ·       TIME      ·       PREV      ·        ·        ·
 F |    ·      STOP?      ·        ·        ·        ·        ·        ·        ·        ·        ·        ·       NEXT      ·        ·        ·
```

| Cells | Count |
|---|---:|
| Named command (including `STOP?`) | 60 |
| `S` status/response | 10 |
| `?` (`C3 D3 E3`) | 3 |
| `·` tested, no visible effect | 183 |
| **Total** | **256** |

Every ROOT byte was sent in the reported sweep, so no ROOT cell is untested. The `?` cells produced an RX response but have no identified action. The `·` cells cover only the CD-playing state. The `41`, `52` and `72` cases show how that baseline can hide state-dependent actions: each did nothing in the sweep but acted in a different state.

## B. CONTEXT matrix (after `0x17`)

The CONTEXT matrix shows bytes sent after a ROOT `0x17` entry and before the `0x07` exit. It is a **selective** map, not a full sweep. Sources:
- the 2026-10-09 targeted `x17` sweep and follow-ups ([v18](../research/notes/kenwood-xs-reverse-engineering-v18.md#L470-L562));
- the legacy tuner-family table converted to `waveform_lsb` ([v18](../research/notes/kenwood-xs-reverse-engineering-v18.md#L380-L417));
- the 2026-10-12 interactive CONTEXT session trials ([note](../research/notes/xs8-context-video-mute-2026-10-12.md)).

```text
        0        1        2        3        4        5        6        7        8        9        A        B        C        D        E        F
   +------------------------------------------------------------------------------------------------------------------------------------------------
 0 |    ·        ?        ?        ?     PRESET10 AUTOTUNE    ·       EXIT    PHONO   VOLSTOP     ·        ·        ?        ·        ·        ·
 1 |    ·        ?        ?        ?     PRESET1   P-SCAN     ·        ?      TRACK1     ?        ?        ·        ?        ·        ·        ·
 2 |    ·        ?        ?        ?     PRESET2    MEM?      ·        ?        ?        ?        ?        ·        ?        ·        ·        ·
 3 |    ·        ?        ?        ?     PRESET3     ·        ·        ?        ?     PWR-OFF     ?        ·        ?        ·        ·        ·
 4 |    ·        ?      PAUSE      ?     PRESET4     ·        ·        ?        ?     DISPTEST    ?        ·        ?        ·        ·        ·
 5 |    ·        ?        ?        ?     PRESET5     ·        ·        ?        ?        ?        ?        ·        ?        ·        ·        ·
 6 |    ·        ?        S        ?     PRESET6     ·        ·        ·        ?     CDDIRECT    ?        ·        ?        ·        ·        ·
 7 |    ·        ?        ?        ?     PRESET7  FREQENT     ·        ·       TV1       ?        ?        ·        ?        ·        ·        ·
 8 |    ·        ?        ?        ?     PRESET8     ?        ·        ?       TV2       ?      DISC+      ·        ?        ·        ·        ·
 9 |    ·        ?        ?        ?     PRESET9     ?        ·        ?        ?        ?        ?        ·        ?        ·        ·        ·
 A |    ·        ?        ?        ?        S        ·        ·        ?        ?     SURROUND    ·        ·        ?        ·        ·        ·
 B |    ·        ?        ?        ?       BAND      ?       CCRS      ?        ?        ?        ?        ·     PLAY/PS     ·        ·        ·
 C |    ·        ?        ?        ?        ·        ?        ·        ?      SLEEP      ?        ?        ·      REPEAT     ·        ·        ·
 D |    ·        ?        ?        ?        ·        ·        ·        ?       MUTE      ?        ?        ·        ?        ·        ·        ·
 E |    ·        ?        ?        ?      TUNE-   RESTART  RESTART     ?        ?       VOL-      ?        ·        ?        ·        ·        ·
 F |    ·        ?        ?        ?      TUNE+      ?        ·        ?        ?       VOL+      ?        ·        ?        ·        ·        ·
```

| Cells | Count |
|---|---:|
| Named command | 38 |
| `S` status/response | 2 |
| `?` not documented as tested / insufficient | 110 |
| `·` tested, no RX and no physical action recorded | 106 |
| **Total** | **256** |

Most CONTEXT `·` cells come from the `x?0`, `x?B`, `x?D`, `x?E` and `x?F` families. Those were recorded as RX-silent, with "no physical action recorded in the relevant run" ([v18](../research/notes/kenwood-xs-reverse-engineering-v18.md#L512-L561)). They are weaker than the ROOT `·`: the test emphasized RX silence. The service's research session refuses `0x17`, `0x07`, `0xE9` and `0xF9` as in-session bytes, so those cells cannot be probed through it.

## C. Detailed command inventory

The State column gives what the result depends on.

Hazard tags:
- **latch**: the action continues until another byte;
- **lock**: front-panel lock;
- **power**;
- **ctx**: changes command interpretation;
- **submode**: tuner entry mode;
- **side**: input-change side effect.

### C.1 ROOT

| Byte | Mnemonic | Observed behavior | State | Evidence | Hazard | Service use |
|---|---|---|---|---|---|---|
| `0x13` | QUERY | Two RX frames: source (`0x77/37/67/A7/27/47` = TV2/PHONO/TAPE1/TV1/TUNER/CD), then about 110–120 ms later MONITOR (`0xE7` OFF, `0xD7` ON). | ROOT; silent after `0x17`. | **P** 2026-10-11 (all six sources, both MONITOR settings) + H | — | `QUERY_STATUS`; post-context check |
| `0x14` | STAT | Same pair as `0x13` (`1D,18` legacy). Did not release the lock. | Only ROOT+CD+MONITOR OFF tested. | H | — | none |
| `0x15` | UNLOCK* | Released the persistent front-panel lock set by any `LOCK*` byte. When already unlocked it can return the `1D,18` pair. | ROOT. After `0x17` it is P-SCAN instead. | H | — | none |
| `0x17` | CTX | Changes interpretation: `0x13` goes silent and `0x84` becomes PRESET8. `0x07` returns to ROOT. | ROOT. | H; **P** entry/exit lifecycle 2026-10-12 (three sessions) | ctx | volume, MUTE, CONTEXT research session |
| `0x18 0x28 0x38 0x48 0x58 0x68 0x78 0x88 0x98` | TRACK1–TRACK9 | Direct CD track 1–9. | CD selected; can be silent away from CD. ROOT `0x78` with TUNER selected did **not** select VIDEO1 (**P** 2026-10-12). | H | — | none |
| `0x21 0x31 0x51 0x61 0x71 0xA1` | STOP | CD STOP. `0x71` was followed by RX `0x94` (legacy `D6`) about 320 ms later. | CD active. | H | — | none |
| `0x9C` | STOP | CD STOP/CANCEL. `41 → 9C` ended in STOP; `FC → 9C` cancelled the skip **and** stopped playback. Delayed RX `0x94` (`D6`) about 330 ms in an older trial. | CD active. | **P** 2026-10-12 + H | — | none |
| `0xF1` | STOP? | Conflicting reports; see §E. | — | H, conflict | — | none |
| `0x41` | PLAY | CD PLAY indicator lit. | Stopped or startable changer. **No visible effect** when already playing (sweep). | **P** 2026-10-12; H negative | — | none |
| `0x42` | PAUSE | Non-toggle pause. | CD playing. | H | — | none |
| `0x52` | PWR-ON | Standby receiver powered on; the changer powered through the switched AC outlet. | Standby. **No effect** when already on. | **P** 2026-10-12; H negative | power | none |
| `0x62` | PWR-OFF | Receiver powered off; RX `0x62` (legacy `B9`) followed. | Receiver on. | H | power | none |
| `0x6A` / `0x7A` | SEEK- / SEEK+ | Continuous backward/forward seek key-down, ended by `0x72`. | CD operating. | **P** 2026-10-12 (REPL pairs; `7A` also via the hold API) + H | latch | `CD_PRESS SEEK_BACKWARD/FORWARD` |
| `0xEC` / `0xFC` | PREV / NEXT | Track key-down; continues until `0x72`.<br>`FC → 72`: one track at a ~100 ms requested gap, two tracks at ~500 ms.<br>`EC → 72`: restarted the current track; a second pair moved back a track. | CD operating; playback position matters. | **P** 2026-10-12 (REPL + API) + H | latch | `CD_TRACK_NEXT/PREVIOUS`, `CD_PRESS` |
| `0x72` | KEYUP | Ended the active `FC/EC/6A/7A` operation in every tested pair. | Active TRACK/SEEK. **No visible effect** with nothing active (sweep). | **P** 2026-10-12; H negative | — | release for CD taps/holds; lease expiry |
| `0x81` | TUNER | Selected TUNER with no immediate RX. | ROOT. | H | — | none |
| `0x82` | TUNER | Selected TUNER; RX `0x21` (legacy `7B`). | ROOT. | **P** (earlier Web Remote report) + H | — | `SELECT_SOURCE TUNER` |
| `0x83` | PHONO | Selected PHONO; RX `0x31` (`73`). | ROOT. | **P** + H | — | `SELECT_SOURCE PHONO` |
| `0x84` | CD | Selected CD; RX `0x41` (`7D`). After `0x17` it is PRESET8. | ROOT; blocked while locked. | **P** (Web Remote report; 2026-10-12 recovery checks) + H | — | `SELECT_SOURCE CD` |
| `0x85` / `0x86` | TAPE1 | Selected TAPE1; RX `0x61` (`79`). Why two bytes give the same result is unknown. | ROOT. | `85` **P** + H; `86` H | — | `85` = `SELECT_SOURCE TAPE1`; `86` none |
| `0x8A` | DISC+ | Next disc; CD input stays selected. | Changer connected; no effect without the changer. | H | — | none |
| `0xB6` | CCRS | CCRS shown on the display. The first time gave RX `0xE4` (`D8`); the second left CCRS active and gave `0x94` (`D6`). It is a set, not a toggle. | Changer connected. | H | — | none |
| `0xBC` | PLAY/PS | Play/pause toggle. Pausing gave RX `0xA4` (`DA`). Resuming gave `0x84` (`DE`), then `0x41` (`7D`) about 107 ms later. | CD playing or paused. | H | — | none |
| `0xCC` | REPEAT | Repeat toggle; no RX. | CD selected. | H | — | none |
| `0xDA` | +10 | CD +10. | CD selected. | H | — | none |
| `0xE5` / `0xE6` | RESTART | Restarts the current disc from the beginning. | Changer connected. | H | — | none |
| `0xEA` | TIME | Cycles the time display (single/total, played/remaining). | CD selected. | H | — | none |
| `0x35 0x36 0x45 0x46 0x55 0x56 0x65 0x66 0x67 0x75 0x76 0x77 0xA5 0xA6 0xD5 0xD6` | LOCK* | Persistent front-panel lock; `0x84` is blocked.<br>Cleared by `0x15` or by standby → wake.<br>Not cleared by `0x07`, `0x14`, `0x1F` or `0x57`.<br>`0x13` still answers while locked. | ROOT. **No lock** after `0x17` (where `0x75` = FREQENT). | H | lock | excluded |
| `0xC3 0xD3 0xE3` | ? | Immediate RX `0x94` (`D6`) after about 3.8, 3.6 and 4.5 ms; no action identified. | Changer connected. | H | — | none |

### C.2 CONTEXT (after `0x17`)

| Byte | Mnemonic | Observed behavior | State | Evidence | Hazard | Service use |
|---|---|---|---|---|---|---|
| `0x07` | EXIT | Returns to ROOT; `0x13` answers again. It does **not** release the ROOT lock. | After `0x17`. | H; **P** 2026-10-12: three exits, each `0x07` **E** `TX_OK`/`MATCH` on one attempt, followed by ROOT query, front-panel and `SELECT_SOURCE CD` recovery checks | — | every context transaction |
| `0x04`, `0x14 0x24 0x34 0x44 0x54 0x64 0x74 0x84 0x94` | PRESET10, PRESET1–PRESET9 | Tuner presets CH10 and CH1–CH9. | Tuner available. | H | side (tuner selected) | none |
| `0x05` | AUTOTUNE | Auto-tune toggle. | Tuner. | H | — | none |
| `0x15` | P-SCAN | Preset scan start/stop. | Tuner. | H | latch | none |
| `0x25` | MEM? | Memory/channel direct-entry behavior; exact role uncertain. | Tuner. | H, partly I | submode | none |
| `0x75` | FREQENT | Enters direct frequency entry. The digit, completion and cancel maps are incomplete. | Tuner. | H | submode | none |
| `0xB4` | BAND | Band AM → FM1 → FM2. | Tuner. | H | — | none |
| `0xE4` / `0xF4` | TUNE- / TUNE+ | Tune down / up. | Tuner. | H | — | none |
| `0x08` | PHONO | PHONO input selected. | After `0x17`. | H | — | none |
| `0x78` | TV1 | VIDEO1 / TV1 selected. The follow-up query reported `source_inferred=TV1` (I). | Entered from ROOT; exited with `0x07`. Starting input not recorded. Human-paced gaps. | **P** 2026-10-12 | — | research session only; production blocked |
| `0x88` | TV2 | VIDEO2 / TV2 selected; the follow-up query reported `source_inferred=TV2` (I). | Same as TV1. | **P** 2026-10-12 | — | research session only; production blocked |
| `0xD8` | MUTE | MUTE **toggle**: muted (production run) and unmuted while muted (2026-10-12). No mute-state readback exists. | Receiver on. | **P** both directions + H | — | production `MUTE` (`17 → D8 → 07`) |
| `0x39` | PWR-OFF | Receiver off; RX `0x62` (`B9`) at +10.2 ms. A later `0x13` answered `1D,18`, so the context appeared reset. That reset is a hypothesis. | Receiver on. | H; reset I | power | none |
| `0x49` | DISPTEST | All display segments lit; a later `0x13` restored the display. | After `0x17`. | H | latch (display) | none |
| `0x69` / `0xA9` / `0xC8` | CDDIRECT / SURROUND / SLEEP | Toggles. | Receiver on. | H | — | none |
| `0xE9` / `0xF9` | VOL- / VOL+ | Continuous volume down/up until `0x09`. | Receiver on. | H; implemented transaction | latch | production volume (`17 → E9/F9 … 09 → 07`); refused in research session |
| `0x09` | VOLSTOP | Stops a continuous volume change. When isolated (no active change), no RX. | Active volume change. | H | — | production volume; startup recovery (experimental) |
| `0x18` | TRACK1 | CD track 1 (repeated selection); `0x13` stays silent. | CD. | H | — | none |
| `0x42` | PAUSE | CD pause (non-toggle). | CD playing. | H | — | none |
| `0x8A` | DISC+ | Next disc, but the input **switches to TUNER**. Reproduced; in ROOT the input stays CD. | Changer connected. | H | side | none |
| `0xB6` | CCRS | CCRS; RX `0xE4` (`D8`) at +3.6 ms; tuner selection observed afterwards. | Changer connected. | H | side | none |
| `0xBC` | PLAY/PS | Play/pause toggle; RX `0x84` (`DE`) on play, `0xA4` (`DA`) on pause; TUNER input selection observed. | CD. | H | side | none |
| `0xCC` | REPEAT | Repeat toggle; no RX. | CD. | H | — | none |
| `0xE5` / `0xE6` | RESTART | Restarts the disc; input switches to TUNER. This is **not** evidence of a CH8 selection. | Changer connected. | H | side | none |

The CD bytes in ROOT and CONTEXT share a primary action. That overlap does **not** show they are the same command: the input side effects differ ([v18 cross-context table](../research/notes/kenwood-xs-reverse-engineering-v18.md#L537-L551)).

## D. Status and response bytes

### D.1 Placed `S` cells

| `waveform_lsb` | Legacy RX label | Meaning observed | Context | TX test of the same byte |
|---|---|---|---|---|
| `0x27` | `1B` | First query response: TUNER | ROOT, after `0x13` | ROOT sweep: no visible effect |
| `0x37` | `13` | First query response: PHONO | ROOT | ROOT sweep: no visible effect |
| `0x47` | `1D` | First query response: CD | ROOT | ROOT sweep: no visible effect |
| `0xA7` | `1A` | First query response: TV1 | ROOT | ROOT sweep: no visible effect |
| `0xE7` | `18` | Second query response: MONITOR OFF | ROOT | ROOT sweep: no visible effect |
| `0xD7` | `14` | Second query response: MONITOR ON | ROOT | ROOT sweep: no visible effect |
| `0x94` | `D6` | Changer report: after STOP (`71/9C`), immediately after `C3/D3/E3`, on the second `B6`, and on physical STOP | ROOT | ROOT sweep: no visible effect. In CONTEXT it is PRESET9. |
| `0xE4` | `D8` | Changer report on the first CCRS activation | ROOT; also seen in CONTEXT | ROOT sweep: no visible effect. In CONTEXT it is TUNE-. |
| `0xA4` | `DA` | Changer report on pause via `0xBC` | ROOT and CONTEXT | ROOT sweep: no visible effect. CONTEXT: no visible effect (older tuner pass). |
| `0x53` | `35` | Report seen **once** after continuous `0xEC`; meaning and cause unverified | ROOT | ROOT sweep: no visible effect |
| `0x62` (CONTEXT cell) | `B9` | Report after the context `0x39` power-off | CONTEXT | CONTEXT TX of `0x62` **not tested** |

The source and MONITOR names are interpretations checked against the display. The changer reports have associations but no established field meanings ([registry RX table](../research/COMMAND_REGISTRY.md#rx-registry-preserve-archived-labels)).

### D.2 Bytes that are both a TX command and a known RX value

These cells show the command. The coincidence is **recorded, not explained**. Never treat a received frame as evidence that its byte was transmitted, and never replay an RX value as a command.

| `waveform_lsb` | TX command (context) | Same value observed as RX (legacy label) |
|---|---|---|
| `0x21` | STOP (ROOT) | TUNER source broadcast (`7B`) |
| `0x31` | STOP (ROOT) | PHONO source broadcast (`73`) |
| `0x61` | STOP (ROOT) | TAPE1 source broadcast (`79`) |
| `0x71` | STOP (ROOT) | TV2 source broadcast (`71`) |
| `0xA1` | STOP (ROOT) | TV1 source broadcast (`7A`) |
| `0x41` | PLAY (ROOT) | CD source broadcast (`7D`) |
| `0x67` / `0x77` | LOCK* (ROOT) | First query response TAPE1 (`19`) / TV2 (`11`) |
| `0x62` | PWR-OFF (ROOT) | RX after power-off (`B9`) |
| `0x84` | CD (ROOT), PRESET8 (CONTEXT) | Changer play/resume report (`DE`) |
| `0xE4` | TUNE- (CONTEXT) | Changer CCRS report (`D8`) |

The bus monitor alone cannot tell a received source broadcast of `0x41` from a transmitted ROOT PLAY `0x41`. The service separates its own frames by TX timing ([readback attribution](../research/notes/xs8-readback-2026-10-11.md#byte-notation-and-attribution)).

### D.3 Early passive-sniffer labels (not placed)

The 2026-10-07 sniffer printed these labels ([sniffing notes](../research/notes/kenwood-xs8-sniffing-notes.md)):
- front-panel source selections: TV2 `8E`, TV1 `85`, TAPE1 `86`, TUNER `84`, PHONO `8C`, CD `82`;
- TAPE2 alternating `EB/E7`;
- changer reports: play `DE → 7D`, pause `D2`, stop `D6`.

Its stated convention matches `legacy_msb_short1`, yet its six source labels are bitwise complements of the later decoder's labels. Bit-reversing each early source label (without complementing) does give the later `waveform_lsb` values, and turns `EB/E7` into the MONITOR responses `0xD7/0xE7`. The same treatment does **not** reproduce the later changer bytes (`DE`/`D6`). No single conversion is established, so these labels stay out of the matrices, as the [registry](../research/COMMAND_REGISTRY.md#byte-encoding-and-decoder-history) requires. Passive pause `D2` versus active pause `DA` remains open.

## E. Conflicts, uncertain cells and departures from earlier summaries

| Byte | Context | Conflict or uncertainty | Matrix treatment |
|---|---|---|---|
| `0xF1` | ROOT | An earlier map recorded CD STOP ([copy 7](../research/notes/kenwood-xs-reverse-engineering%20copy%207.md#L343-L354)). The later full sweep recorded no visible effect ([v18](../research/notes/kenwood-xs-reverse-engineering-v18.md#L250-L269)). The starting playback states cannot be reconstructed. | `STOP?` |
| `0xC3 0xD3 0xE3` | ROOT | Immediate RX `0x94` only. The `D6` report also follows STOP and CCRS, so it does not identify an action. | `?` |
| `0x14` | ROOT | Equals `0x13` only in ROOT+CD+MONITOR OFF; not proven read-only or equivalent elsewhere. | `STAT` |
| `0x41`, `0x52`, `0x72` | ROOT | A newer positive physical result versus an older no-effect result. The tested states differ (already playing, already on, no active transport), so this is a **state dependency, not a conflict**. | Named; both results in §C.1 |
| `0x38`, `0x48` | ROOT / CONTEXT | Historical labels TAPE1 / MONITOR toggle (state not recorded) versus controlled ROOT+CD tracks 3/4. The `x78/x88` precedent suggests the historical labels were context observations, but neither byte has been tested after `0x17`. | ROOT `TRACK3/4`; CONTEXT `?` |
| `0x78`, `0x88` | ROOT / CONTEXT | Historical VIDEO1/VIDEO2 versus ROOT tracks 7/8, **resolved by state** on 2026-10-12. | ROOT `TRACK7/8`; CONTEXT `TV1/TV2` |
| `0x13` | CONTEXT | No RX and no visible effect, yet it restored the normal display after `0x49`. An effect exists, but its function is unknown. The [coverage map](../research/XS8_COMMAND_COVERAGE.md#2-after-logical-tx-0x17-matrix) classed it as tested no-RX. | `?` |
| `0x06 0x16 0x26 0x86 0x96 0xC6 0xF6` | CONTEXT | v18 states "`x06`–`xA6`, `xC6`, `xD6`, `xF6` had no observed reaction in the tested context" ([v18](../research/notes/kenwood-xs-reverse-engineering-v18.md#L500)). The coverage map left these seven as untested `U`. | `·` (departure from the coverage map) |
| `0x17` | CONTEXT | A nested `0x17` has never been tested, and the service refuses it in the research session. | `?` |
| `0x1F` | CONTEXT | Explicit negative: it did **not** return to ROOT. | `·` |
| `0x25` | CONTEXT | Memory/channel direct entry with mixed submode transitions. | `MEM?` |
| `0x8A` | ROOT | An early note said "previous disc". The v18 correction and A/B show next disc. | `DISC+` |
| `0x15` | ROOT | An early note called it "UNLOCK". v18 restricts it to an observed release (`REL*`): it may be a more general cancel. | `UNLOCK*` |
| `0xBC` | ROOT | Active pause gave RX `DA`, but passive physical PAUSE gave early `D2`. | `PLAY/PS`; RX conflict noted in §D.3 |
| `0x62` | CONTEXT | RX `B9` was observed, but TX `0x62` was not tested in context. | `S` |

### Historical labels of unknown context

The [legacy receiver list](../research/notes/kenwood-xs-reverse-engineering-v18.md#L429-L453) recorded these labels in an uncontrolled "NORMAL" state.

| `waveform_lsb` | Historical label | Controlled ROOT result | CONTEXT result |
|---|---|---|---|
| `0x08` | PHONO | `·` | PHONO (H) |
| `0x18` | CD select | TRACK1 | TRACK1 |
| `0x49` | all display segments | `·` | DISPTEST (H) |
| `0x69` | CD select | `·` | CDDIRECT (H) |
| `0x74` | preset CH7 | `·` | PRESET7 (H) |
| `0xC8` | display toggle | `·` | SLEEP (H) |
| `0xF4` | tune/scan up | `·` | TUNE+ (H) |
| `0x52` | power on | PWR-ON from standby (**P**) | `?` |
| `0x62`, `0x39` | power off | ROOT `0x62` PWR-OFF | `0x39` PWR-OFF |
| `0xF9` / `0xE9` | volume up / down hold | `·` | VOL+ / VOL- |

Most of these match the later CONTEXT results. That is consistent with, but does not prove, the historical tests having been made after `0x17`. The CONTEXT matrix does not use these labels. Library constants (KenwoodXS) are not used as evidence anywhere in this report.

## F. Coverage and confidence summary

| | Named | of which **P** (recent physical) | `S` | `·` | `?` |
|---|---:|---:|---:|---:|---:|
| ROOT | 60 | 14: `13 17 41 52 6A 72 7A 82 83 84 85 9C EC FC` | 10 | 183 | 3 |
| CONTEXT | 38 | 4: `07 78 88 D8` | 2 | 106 | 110 |

The ROOT `·` cells come from a single state (CD selected and playing, unlocked, receiver on). The CONTEXT map is selective, and 110 cells have no documented test. Only the 18 **P** commands have recent physical validation. All other named cells are historical bench observations without archived per-byte logs. The retained serial capture ([scan](../research/captures/kenwood-xs-scan.txt)) stops at legacy candidate `17`, so it cannot verify the full sweep.

## Sources reviewed

- **Historical notes.** [`kenwood-xs-reverse-engineering-v18.md`](../research/notes/kenwood-xs-reverse-engineering-v18.md) was read, including its uncommitted user edits to the ROOT rows `0x41` PLAY, `0x52` POWER ON and `0x62` POWER OFF; it was not modified. Also the earlier versions (`-reviewed`, copies 4 and 7) for historical cells.
- **Recent physical notes.** [Sniffing notes](../research/notes/kenwood-xs8-sniffing-notes.md), [readback 2026-10-11](../research/notes/xs8-readback-2026-10-11.md), [ROOT validation](../research/notes/xs8-root-validation-2026-10-12.md), [CD key release](../research/notes/xs8-cd-key-release-2026-10-12.md), [CD hold API validation](../research/notes/xs8-cd-hold-api-validation-2026-10-12.md), [key-release candidates](../research/notes/xs8-key-release-candidates.md) and [CONTEXT VIDEO/MUTE](../research/notes/xs8-context-video-mute-2026-10-12.md).
- **Research summaries.** [Command registry](../research/COMMAND_REGISTRY.md) and [coverage map](../research/XS8_COMMAND_COVERAGE.md).
- **Protocol docs.** [Protocol](xs8-protocol.md), [source selection](source-selection.md) and [CD transport API](cd-transport-api.md).
- **Service code.** Read only to fill the "Service use" column, never as evidence of receiver behavior: [`xs8_service_core.cpp`](../xs8_service_core.cpp), [`xs8_receiver_state.cpp`](../xs8_receiver_state.cpp) and [`xs8_decode.c`](../xs8_decode.c).
