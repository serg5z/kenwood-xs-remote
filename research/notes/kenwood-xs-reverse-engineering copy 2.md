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
0x1F              -> exit TUNER context
0xDE              -> CD
```


## Reproducible input-selector lock (`0x77`) — 2026-10-08

Clean controlled test on the KR-V6070:

```text
cold/clean state -> front-panel inputs selectable
TX 0x77          -> front-panel input selection LOCKED
TX 0x07          -> still LOCKED
standby -> wake  -> inputs selectable again
TX 0x77          -> front-panel input selection LOCKED again
```

Conclusions from the observed behavior:

- `0x77` is a reproducible trigger for the receiver's front-panel input-selection lock/state.
- The result was reproduced after returning the receiver to a clean selectable-input state.
- `0x07` does **not** release this lock.
- `0x1F` does **not** release this lock.
- Current-code `0x57` does **not** release this lock; do not confuse it with the old-decoder `0x57`.
- `0x15` **does release** the `0x77` lock. The receiver then emitted `0x1D -> 0x18`.
- This is consistent with the old-decoder recovery byte `0x57` mapping to current `0x15` via bit-reverse + invert, the same transform that maps old `0x11` to current `0x77`.
- A receiver standby -> wake cycle clears the lock; full AC power removal is not required.
- Do not yet assign a higher-level protocol name/meaning to `0x77`; only the observable lock behavior is established.

## Context control

``` text
0x17 -> ENTER TUNER context
0x1F -> EXIT TUNER context
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
- `R*` = receiver RX `1D`, then about 110 ms later RX `18`; no visible effect
- `🔒` = front-panel input-selection lock (do not use in normal testing)
- `·` = tested, no observed effect in the tested context
- blank = not established / not recorded as tested here

| hi\\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|---:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** |  |  |  |  |  |  |  |  | · |  |  |  |  |  |  |  |
| **1** |  | · | · | R* | R* | R* | · | · | T1 | · | · | · | · | · | · | · |
| **2** |  |  |  |  |  |  |  |  | T2 | · | · | · | · | · | · | · |
| **3** |  |  |  |  |  |  |  |  | T3 | · | · | · | · | · | · | · |
| **4** |  |  |  |  |  |  |  |  | T4 | · | · | · | · | · | · | · |
| **5** |  |  |  |  |  |  |  |  | T5 | · | · | · | · | · | · | · |
| **6** |  |  |  |  |  |  |  |  | T6 | · | ◀ | · | · | · | · | · |
| **7** |  |  |  |  |  |  |  | 🔒 | T7 | · | ▶ | · | · | · | · | · |
| **8** |  |  |  |  | CD |  |  |  | T8 | · | D◀ | · | · | · | · | · |
| **9** |  |  |  |  |  |  |  |  | T9 | · | · | · | ■ | · | · | · |
| **A** |  |  |  |  |  |  |  |  | · | · | · | · | · | · | · | · |
| **B** |  |  |  |  |  |  |  |  | · | · | · | · | Ⅱ | · | · | · |
| **C** |  |  |  |  |  |  |  |  | · | · | · | · | · | · | · | · |
| **D** |  |  |  |  |  |  |  |  | · | · | +10 | · | · | · | · | · |
| **E** |  |  |  |  |  |  |  |  | · | · | TIME | · | |◀ | · | · | · |
| **F** |  |  |  |  |  |  |  |  | · | · | · | · | ▶▶… | · | · | · |

### Context / caveats for the map

- `18`..`98` select CD tracks 1..9 **when CD is the selected input**. After changing away from CD they were silent; selecting CD again restored the behavior.
- `6A` = skip back, `7A` = skip forward, `8A` = previous disc, `DA` = +10, `EA` changes the CD time-display mode.
- `9C` stops playback and also stops the continuous forward-skip behavior triggered by `FC`.
- `BC` toggles pause/play. Entering pause produced RX `DA`; leaving pause produced RX `DE`, then receiver RX `7D`.
- `EC` behaves as previous track (and may first restart the current track depending on playback position, as normal CD previous-track behavior does).
- `FC` starts repeated/continuous forward track skipping; treat it cautiously.
- `13`, `14`, and `15` each produced receiver RX `1D`, then about 110 ms later RX `18`, with no visible effect. Disconnect tests showed both RX frames originate from the receiver, not the CD changer.
- `15` also recovered the front-panel input-selection lock caused by `77`, but `15` produces the same `1D -> 18` RX pair even from normal state, so do not label it simply as an unlock command.
- `77` reproducibly locks front-panel input selection. We are deliberately not using it further in normal exploration.
- `84` is the known logical CD-select command and is useful as a safe context probe; receiver RX `7D` confirms CD selection.

This map records observed behavior, not assumed protocol semantics. A byte may be context-sensitive.
