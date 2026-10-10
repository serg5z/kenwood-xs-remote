# Kenwood XS Reverse-Engineering Notes

Status: work in progress\
Target: Kenwood KR-V6070 / SYSTEM CONTROL (XS)

This file records **physically observed behavior**. Unknown commands are
intentionally left unknown rather than inferred.

## Key finding: XS is context-sensitive

A byte does **not** have one global meaning. Strongest probe: `0xDE`.

  -----------------------------------------------------------------------
  Context                             `0xDE` behavior
  ----------------------------------- -----------------------------------
  NORMAL / receiver                   Select CD; RX `0x7D`

  `0x17` TUNER context                Select preset CH8

  Numeric/direct-entry contexts       Numeric meaning observed; exact
                                      mapping still needs a clean
                                      isolated pass
  -----------------------------------------------------------------------

Working model: **context + byte -\> operation/data**. Audio input and XS
command context are separate state variables.

Example:

``` text
0x17              -> XS context = TUNER
front-panel CD    -> audio input = CD; XS context remains TUNER
0xDE              -> preset CH8
0x07              -> exit TUNER context
0xDE              -> CD
```


## Reproducible front-panel control lock (`0x67` / `0x77`) — updated 2026-10-09

Clean controlled tests on the KR-V6070 established a receiver-side front-panel lock state:

```text
TX 0x67          -> all front-panel buttons LOCKED
TX 0x67 again    -> remains LOCKED (not a toggle)
TX 0x77          -> remains LOCKED
TX 0x15          -> front panel UNLOCKED

TX 0x77          -> all front-panel buttons LOCKED
TX 0x77 again    -> remains LOCKED (not a toggle)
TX 0x67          -> remains LOCKED
TX 0x15          -> front panel UNLOCKED

TX 0x67, 0x77    -> LOCKED
TX 0x15          -> fully UNLOCKED
```

Conclusions from the observed behavior:

- Both `0x67` and `0x77` reproducibly lock **all front-panel buttons**, not only input selection.
- Neither command is a toggle, and neither releases the state established by the other.
- `0x15` reliably releases the lock produced by `0x67`, `0x77`, or by sending both setters before the release.
- `0x14` does **not** release the lock.
- `0x07`, `0x1F`, and current-code `0x57` also do **not** release the lock.
- `0x13` continues to return normal receiver source/monitor status while the front panel is locked, so this lock is independent of the state visible through `0x13`.
- A standby -> wake cycle also clears the lock.
- Externally, the simplest current model is one front-panel lock state with two observed setter commands (`0x67`, `0x77`) and one observed release command (`0x15`). This does **not** prove the internal implementation is a single bit.
- The reason two different setter bytes exist is still unknown.

## Context control

``` text
0x17 -> ENTER TUNER context
0x07 -> EXIT TUNER context
0x1F -> does NOT exit TUNER context
```

The old `0x11 = SOURCE LOCK` interpretation is not established and
should be discarded for now.\
Likewise `0x57` is not a generic recovery command; it has a real tuner
function.

## `0x17` TUNER context

  Byte   Observed action
  ------ --------------------------
  `D0`   TUNE UP
  `D1`   PRESET CH7
  `D2`   BAND: AM -\> FM1 -\> FM2
  `D3`   PRESET CH3
  `D4`   no visible effect
  `D5`   PRESET CH5
  `D6`   PRESET CH9
  `D7`   PRESET CH1
  `D8`   TUNE DOWN
  `D9`   PRESET CH6
  `DA`   no visible effect
  `DB`   PRESET CH2
  `DC`   no visible effect
  `DD`   PRESET CH4
  `DE`   PRESET CH8
  `DF`   PRESET CH10

Preset mapping:

``` text
CH1  -> D7
CH2  -> DB
CH3  -> D3
CH4  -> DD
CH5  -> D5
CH6  -> D9
CH7  -> D1
CH8  -> DE
CH9  -> D6
CH10 -> DF
```

Other tuner/direct-entry observations:

``` text
51 -> DIRECT FREQUENCY ENTRY
57 -> TUNER PRESET SCAN START/STOP
5B -> DIRECT CHANNEL ENTRY
```

Numeric behavior including `DE -> 8` and `DF -> 0` was observed during
direct-entry exploration, but the submode transitions were mixed; remap
cleanly before treating the full numeric table as final.

## NORMAL / receiver observations

  Byte   Observed action                  Notes
  ------ -------------------------------- ------------------------------------
  `11`   changes state/context            tentative; not established as lock
  `17`   ENTER TUNER context              confirmed
  `1F`   EXIT TUNER context               confirmed
  `3E`   PHONO                            confirmed
  `51`   DIRECT FREQUENCY ENTRY           observed
  `57`   preset scan START/STOP           context-sensitive
  `5B`   DIRECT CHANNEL ENTRY             observed
  `5E`   TAPE1                            confirmed
  `60`   VOLUME UP / continuous hold      hazardous
  `63`   POWER OFF                        receiver emitted `B9`
  `68`   VOLUME DOWN / continuous hold    hazardous
  `69`   select CD input                  does not exit `17`
  `6D`   all display segments/labels ON   not a toggle
  `B5`   POWER ON                         confirmed
  `B9`   POWER OFF                        confirmed
  `BE`   TUNER                            confirmed
  `D0`   TUNE / SCAN UP                   confirmed
  `D1`   PRESET CH7                       confirmed
  `DE`   CD                               RX `7D` in NORMAL context
  `E1`   VIDEO1                           confirmed
  `E3`   TAPE1                            confirmed
  `E7`   CD                               confirmed
  `EB`   PRESET CH8 ?                     tentative; repeat cleanly
  `EC`   DISPLAY OFF / ON toggle          confirmed
  `ED`   MONITOR ON / OFF toggle          confirmed
  `EE`   VIDEO2                           confirmed
  `EF`   PHONO                            confirmed

## Known front-panel source broadcasts

``` text
TV2    -> 71
PHONO  -> 73
TAPE1  -> 79
TV1    -> 7A
TUNER  -> 7B
CD     -> 7D
```

Receiver-directed commands and front-panel broadcasts are distinct:

``` text
receiver command       broadcast/status
PHONO  3E              73
TAPE1  5E              79
TUNER  BE              7B
CD     DE              7D
```

No simple global transform is established.

## Physical CD changer observations

``` text
PLAY from STOP:   changer -> DE, then receiver -> 7D
PAUSE:            changer -> D2
PLAY from PAUSE:  changer -> DE, then receiver -> 7D
STOP:             changer -> D6
```

Known KenwoodXS definitions consistent with observations:

``` text
AE -> CD_DISC
C0 -> CD_NEXT_TRACK
C2 -> CD_PLAY_PAUSE
C6 -> CD_STOP
C8 -> CD_PREV_TRACK
DE -> CD_START
```

## Continuous / hazardous behavior

``` text
60 -> VOLUME UP / HOLD
68 -> VOLUME DOWN / HOLD
```

These can continue changing volume after one transmitted frame. A VOL
LED later remained blinking and did not clear through ordinary
front-panel interaction; restarting the receiver cleared it.

Until release semantics are understood, exclude `60` and `68` from
unattended scans.

``` text
63 -> POWER OFF
6D -> force all display segments/labels ON
```

`6D` is not a toggle; a subsequent normal display update can restore the
normal display.

## Interesting structural patterns

Several observed pairs differ by `0x08`:

``` text
D0 -> TUNE UP
D8 -> TUNE DOWN

60 -> VOLUME UP / HOLD
68 -> VOLUME DOWN / HOLD

17 -> ENTER TUNER context
1F -> EXIT TUNER context
```

This is worth tracking but is not yet a proven general protocol rule.

Not every byte changes meaning between NORMAL and `0x17`:

  Byte   NORMAL         `0x17` TUNER
  ------ -------------- --------------
  `D0`   TUNE/SCAN UP   TUNE UP
  `D1`   PRESET CH7     PRESET CH7
  `DE`   CD             PRESET CH8

So `0x17` appears to modify at least part of the command interpretation
rather than replace all 256 meanings.

## Current working model

``` text
NORMAL
  |
  +-- 17 --> TUNER context
  |             |
  |             +-- D0/D8 tuning
  |             +-- D* presets
  |             +-- D2 band
  |             +-- 51 direct frequency entry
  |             +-- 5B direct channel entry
  |
  <-- 1F -------+
```

Audio source selection is not identical to command-context selection.

## Bus / framing reference

Wiring:

``` text
D2 TX -> 2.2k -> DATA
D3 TX -> 2.2k -> BUSY
A0 = DATA RX
A5 = BUSY RX
common GND
```

Mapping:

``` text
D2/A0 = DATA / SDAT
D3/A5 = BUSY / CTRL
idle low
```

Nominal transmitter timing:

``` cpp
const uint16_t LEAD_US  = 1150;
const uint16_t START_US = 9800;
const uint16_t ONE_US   = 4900;
const uint16_t ZERO_US  = 9800;
const uint16_t HIGH_US  = 4900;
const uint16_t END_US   = 750;
```

Electrical TX model:

-   HIGH = OUTPUT HIGH
-   LOW = INPUT / high-Z
-   release bus after frame

## Next targeted work

1.  Cleanly map digits in `51` DIRECT FREQUENCY ENTRY.
2.  Cleanly map digits in `5B` DIRECT CHANNEL ENTRY.
3.  Find explicit exit/cancel behavior for those submodes.
4.  Recheck `EB -> CH8` from a clean state.
5.  Determine the real semantics of `11`.
6.  Find release/stop for `60` / `68` volume hold.
7.  Annotate `DE` in scanner output as `[CONTEXT PROBE]`, not always
    `[CD cmd]`.

The full `00-FF` brute-force sweep has been completed. Future work
should be targeted and context-aware.

## Root-context additions — receiver / tape-system observations

The following commands were observed while XS remained in **root context**. They must not be interpreted as a separate TAPE context merely because some actions involve TAPE1/cassette functions.

| TX | Observed result | Status |
|---|---|---|
| `86` | receiver RX `79` `[TAPE1]` | TAPE1 select observed |
| `85` | receiver RX `79` `[TAPE1]` | TAPE1 select observed |
| `B6` | **CCRS shown on receiver display**; RX `D8` | CCRS effect confirmed; meaning of `D8` not yet assigned |
| `E6` | appears to play from beginning | tentative |
| `E5` | appears to play from beginning | tentative |
| `14` | RX `1D`, then about 110 ms later `18` | same receiver response family as prior root tests |
| `13` | RX `1D`, then about 110 ms later `18` | same receiver response family as prior root tests |
| `C3` | RX `D6` | action semantics not yet assigned |
| `D3` | RX `D6` | action semantics not yet assigned |
| `E3` | RX `D6` | action semantics not yet assigned |
| `42` | PAUSE | non-toggle pause observed |
| `62` | RX `B9`; receiver powers off | POWER OFF |
| `82` | RX `7B` `[TUNER]` | TUNER select |
| `21` | STOP | observed |
| `31` | STOP | observed |
| `51` | STOP | observed in this root test; note older observations assigned other behavior, so preserve as context/state-sensitive evidence |
| `61` | STOP | observed |
| `71` | STOP | observed |
| `A1` | STOP | observed |
| `F1` | STOP | observed |

Important: `B6` is no longer merely a `CCRS?` hypothesis. **CCRS was visibly shown on the display.** What RX `D8` represents remains unknown.

The repeated STOP observations on low nibble `1` (`21/31/51/61/71/A1/F1`) suggest a structural family, but no general rule is claimed yet.

## XS8 physical waveform shorthand — 2026-10-08

For quick timing sketches, use one character per approximately 5 ms:

```text
L   ~=  5 ms LOW
LL  ~= 10 ms LOW
H   ~=  5 ms HIGH
HH  ~= 10 ms HIGH
```

Current logical TX convention (LSB-first):

```text
logical 0:  L H     # LOW ~5 ms,  HIGH ~5 ms
logical 1:  LLH     # LOW ~10 ms, HIGH ~5 ms
```

Frame start, schematically:

```text
BUSY:  H-----------------------------------------------
DATA:    HH  [bit][bit][bit][bit][bit][bit][bit][bit]

BUSY rises first; after ~1.15 ms DATA rises for the ~9.8 ms start pulse.
```

Example logical bits `0 1 0 0 1 1 0 1` (LSB-first):

```text
HH | LH | LLH | LH | LH | LLH | LLH | LH | LLH
```

Important: this is the **current logical TX notation**. The older/legacy decoder convention labels the physical short/long LOW intervals with the opposite bit values. Conversion between old and current byte notation is bit-reverse + invert.

## 16x16 command map (current logical TX bytes)

Compact map of the current-byte space. Row = high nibble, column = low nibble.

Legend:
- `Tn` = direct CD track select n (observed only with CD selected)
- `◀` / `▶` = CD skip back / forward
- `D◀` = previous disc
- `+10` = CD +10
- `TIME` = CD time-display mode
- `■` = stop
- `Ⅱ` = pause/play toggle
- `|◀` = previous track
- `▶▶…` = continuous/repeated forward skipping; `9C` stops it
- `STAT` = receiver status query (`13`); first RX identifies selected source, second RX reports monitor state
- `🔒` = front-panel control lock (all front-panel buttons)
- `Ⅱ!` = PAUSE, non-toggle
- `OFF` = POWER OFF; receiver RX `B9`
- `TUN` = TUNER select; receiver RX `7B`
- `TP1` = TAPE1 select; receiver RX `79`
- `CCRS` = CCRS visibly shown on receiver display; RX `D8`
- `→D6` = receiver RX `D6`; action semantics not yet assigned
- `PLAY0?` = appears to play from beginning; tentative
- `■†` = STOP observed in this root test, but older observations assigned another behavior to the same byte
- `·` = tested, no observed effect in the tested context
- blank = not established / not recorded as tested here

| hi\\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|---:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** |  |  |  |  |  |  |  |  | · |  |  |  |  |  |  |  |
| **1** |  | · | · | STAT | R* | R* | · | →TUN | T1 | · | · | · | · | · | · | · |
| **2** |  | ■ |  |  |  |  |  |  | T2 | · | · | · | · | · | · | · |
| **3** |  | ■ |  |  |  |  |  |  | T3 | · | · | · | · | · | · | · |
| **4** |  |  | Ⅱ! |  |  |  |  |  | T4 | · | · | · | · | · | · | · |
| **5** |  | ■† |  |  |  |  |  |  | T5 | · | · | · | · | · | · | · |
| **6** |  | ■ | OFF |  |  |  |  | 🔒 | T6 | · | ◀ | · | · | · | · | · |
| **7** |  | ■ |  |  |  |  |  | 🔒 | T7 | · | ▶ | · | · | · | · | · |
| **8** |  |  | TUN |  | CD | TP1 | TP1 |  | T8 | · | D◀ | · | · | · | · | · |
| **9** |  |  |  |  |  |  |  |  | T9 | · | · | · | ■ | · | · | · |
| **A** |  | ■ |  |  |  |  |  |  | · | · | · | · | · | · | · | · |
| **B** |  |  |  |  |  |  | CCRS |  | · | · | · | · | Ⅱ | · | · | · |
| **C** |  |  |  | →D6 |  |  |  |  | · | · | · | · | · | · | · | · |
| **D** |  |  |  | →D6 |  |  |  |  | · | · | +10 | · | · | · | · | · |
| **E** |  |  |  | →D6 |  | PLAY0? | PLAY0? |  | · | · | TIME | · | |◀ | · | · | · |
| **F** |  | ■ |  |  |  |  |  |  | · | · | · | · | ▶▶… | · | · | · |

### Context / caveats for the map

- `18`..`98` select CD tracks 1..9 **when CD is the selected input**. After changing away from CD they were silent; selecting CD again restored the behavior.
- `6A` = skip back, `7A` = skip forward, `8A` = previous disc, `DA` = +10, `EA` changes the CD time-display mode.
- `9C` stops playback and also stops the continuous forward-skip behavior triggered by `FC`.
- `BC` toggles pause/play. Entering pause produced RX `DA`; leaving pause produced RX `DE`, then receiver RX `7D`.
- `EC` behaves as previous track (and may first restart the current track depending on playback position, as normal CD previous-track behavior does).
- `FC` starts repeated/continuous forward track skipping; treat it cautiously.
- `13` is now identified as a receiver status query in root context; see the dedicated section below. `14` and `15` previously produced receiver RX `1D`, then about 110 ms later RX `18`, with no visible effect in the tested root/CD state.
- `15` is the observed front-panel UNLOCK command: it releases locks set by `67`, `77`, or both. `14` does not unlock. `15` may still produce the familiar status-like RX pair when sent from an already-unlocked state.
- `67` and `77` reproducibly lock all front-panel buttons. Both are setters, not toggles; their higher-level distinction remains unknown.
- `84` is the known logical CD-select command and is useful as a safe context probe; receiver RX `7D` confirms CD selection.

This map records observed behavior, not assumed protocol semantics. A byte may be context-sensitive.

## `x13` receiver status query — 2026-10-09

In root context, `x13` produces two receiver responses. The first response tracks the selected input; the second tracks MONITOR state. No visible action from `x13` itself has been observed.

| Selected input | first RX | second RX (MONITOR OFF) |
|---|---:|---:|
| VIDEO2 / TV2 | `11` | `18` |
| PHONO | `13` | `18` |
| TAPE1 | `19` | `18` |
| VIDEO1 / TV1 | `1A` | `18` |
| TUNER | `1B` | `18` |
| CD | `1D` | `18` |

With VIDEO1 selected, toggling MONITOR gave a clean A/B result:

```text
MONITOR OFF  -> x13 -> 1A, then ~115 ms later 18
MONITOR ON   -> x13 -> 1A, then ~115 ms later 14
MONITOR OFF  -> x13 -> 1A, then ~115 ms later 18
```

Thus the current working interpretation is:

```text
x13 -> [selected-source status] -> ~110-120 ms -> [monitor/routing status]

second RX 18 = MONITOR OFF
second RX 14 = MONITOR ON
```

The first RX also has a strong structural relationship to the receiver's spontaneous front-panel source broadcasts:

```text
VIDEO2: 71 -> 11
PHONO:  73 -> 13
TAPE1:  79 -> 19
VIDEO1: 7A -> 1A
TUNER:  7B -> 1B
CD:     7D -> 1D
```

Observed relationship: the source-identifying low bits are preserved while the upper message-class bits differ (`7x` broadcast versus `1x` query response). Do not yet assume that the receiver literally computes one byte from the other.

`x13` is also useful as a root-vs-`x17` context probe:

```text
root context:       x13 -> 1?, then 18/14 depending on state
after x17 context:  x13 -> no RX; no visible reaction observed
```

`x17` silently enters the observed tuner command context. `x07` returned command interpretation to root: after `x07`, `x84` again selected CD and returned RX `7D`. The first `x13` immediately after `x07` once returned `1B,18`; a subsequent `x13` returned the steady-state source-dependent signature. `x1F` was tested and did **not** return to root.

## 16x16 command map — `0x17` context (observed tuner behavior)

Separate map for bytes observed after entering the `0x17` context. Keep this distinct from the NORMAL/root map: the same byte can have a different meaning here. The label **TUNER** describes the observed behavior; the higher-level semantics of `0x17` itself are still treated cautiously.

Legend:
- `P1`..`P10` = tuner preset channel
- `TU+` / `TU-` = tune up / down
- `BAND` = AM -> FM1 -> FM2
- `DIR-F` = direct frequency entry
- `SCAN` = tuner preset scan start/stop
- `DIR-C` = direct channel entry
- `·` = tested, no visible effect in this context
- blank = not established / not recorded here

| hi\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|---:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** |  |  |  |  |  |  |  | EXIT |  |  |  |  |  |  |  |  |
| **1** |  |  |  |  |  |  |  | CTX |  |  |  |  |  |  |  | NOEXIT |
| **2** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **3** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **4** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **5** |  | DIR-F |  |  |  |  |  | SCAN |  |  |  | DIR-C |  |  |  |  |
| **6** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **7** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **8** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **9** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **A** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **B** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **C** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **D** | TU+ | P7 | BAND | P3 | · | P5 | P9 | P1 | TU- | P6 | · | P2 | · | P4 | P8 | P10 |
| **E** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| **F** |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |

### `0x17`-context caveats

- The `D*` row is the strongest cleanly mapped tuner family: `D0/D8` tune, `D2` band, and the remaining observed `D*` values map presets 1..10 as shown.
- `51`, `57`, and `5B` were observed as direct-frequency entry, preset scan, and direct-channel entry respectively. Numeric behavior inside those submodes was explored but is not yet clean enough to place as final semantics in this matrix.
- `DE` is the clearest context probe: in NORMAL/root it selects CD, while in this context it selects tuner preset CH8.
- `17` enters the observed tuner context silently. `07` returned command interpretation to root and is shown as `EXIT`. `1F` was explicitly tested and did **not** return to root (`NOEXIT`).
