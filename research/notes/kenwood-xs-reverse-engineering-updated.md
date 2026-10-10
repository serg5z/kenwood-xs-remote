# Kenwood XS Reverse-Engineering Notes

Status: work in progress  
Target: Kenwood KR-V6070 / SYSTEM CONTROL (XS)  
Last major update: 2026-10-09

This file records **physically observed behavior**. Unknown commands are intentionally left unknown rather than inferred. In particular, an observed receiver lock state is not assumed to mean that the transmitted byte is semantically a `LOCK` command.

## Current byte convention

Unless a section is explicitly marked **legacy**, bytes in this document use the current logical TX convention:

- LSB first
- logical `0` = short LOW (~4.9 ms) + HIGH (~4.9 ms)
- logical `1` = long LOW (~9.8 ms) + HIGH (~4.9 ms)

Older KenwoodXS/library captures used the opposite short/long bit labels and MSB-first presentation. Conversion is:

```text
current = reverse8(~legacy)
legacy  = reverse8(~current)
```

Do not compare legacy and current byte values directly.

## Key finding: XS is context-sensitive

A byte does **not** have one global meaning. Audio input and XS command context are separate state variables.

The strongest known context transition is current byte `x17`:

```text
ROOT
x17
x13 -> no RX
```

Inside the resulting context, commands show a strong tuner-like interpretation. For example current `x84`, which selects CD in ROOT, selects tuner preset CH8 after `x17`.

Current conservative interpretation:

```text
x17 -> ENTER subcommand/device context
       observed command map is tuner-like
       higher-level meaning is not yet proven

x07 -> returns from the observed x17 context to ROOT
x1F -> does NOT return to ROOT
```

Earlier notes called `x17` definitively `ENTER TUNER`. That is now treated as an over-interpretation. A generic device/component context remains a live hypothesis.

A full one-byte `00-FF` sweep in controlled `ROOT + CD + playing` state found **no second obvious context-enter byte** analogous to `x17`. This does not prove that a CD-specific context does not exist: it may require a sequence, another participant, or additional bus state.

## Electrical / bus reference

Verified working Arduino Micro 5 V setup:

```text
TRS sleeve = GND
TRS tip    = BUSY
TRS ring   = DATA

D2 TX -> 2.2k -> DATA
D3 TX -> 2.2k -> BUSY
A0 = DATA RX
A5 = BUSY RX
common GND
```

Current working electrical model:

- idle LOW
- transmit HIGH by `OUTPUT HIGH`
- release line with `INPUT` / high-Z
- receiver/changer/Arduino HIGH ADC levels differ, but the setup is working reliably
- do **not** replace this with Internet open-drain/pull-up descriptions; those are a different electrical model

Observed/nominal timing:

```text
BUSY rises
~1.15 ms later DATA start HIGH
DATA start HIGH ~9.8 ms
each data bit: LOW 4.9 or 9.8 ms, then HIGH ~4.9 ms
```

Shorthand:

```text
L   ~=  5 ms LOW
LL  ~= 10 ms LOW
H   ~=  5 ms HIGH
HH  ~= 10 ms HIGH

logical 0 = LH
logical 1 = LLH
```

Current TX timing constants:

```cpp
const uint16_t LEAD_US  = 1150;
const uint16_t START_US = 9800;
const uint16_t ONE_US   = 4900;
const uint16_t ZERO_US  = 9800;
const uint16_t HIGH_US  = 4900;
const uint16_t END_US   = 750;
```

Current transmitter waits for BUSY LOW and stable for 500 us before claiming the bus. Current experimental tail releases DATA first, waits ~750 us, then releases BUSY. This tail differs from the originally observed Kenwood tail and remains an experiment.

## `x13` receiver status query

In ROOT, `x13` produces two receiver responses. The first tracks selected source; the second tracks MONITOR state.

| Selected input | first RX | second RX, MONITOR OFF |
|---|---:|---:|
| VIDEO2 / TV2 | `11` | `18` |
| PHONO | `13` | `18` |
| TAPE1 | `19` | `18` |
| VIDEO1 / TV1 | `1A` | `18` |
| TUNER | `1B` | `18` |
| CD | `1D` | `18` |

Clean MONITOR A/B:

```text
MONITOR OFF -> x13 -> 1A, then ~115 ms -> 18
MONITOR ON  -> x13 -> 1A, then ~115 ms -> 14
MONITOR OFF -> x13 -> 1A, then ~115 ms -> 18
```

Working interpretation:

```text
x13 -> [selected-source status] -> ~110-120 ms -> [monitor/routing status]

18 = MONITOR OFF
14 = MONITOR ON
```

Known spontaneous/front-panel source broadcasts:

```text
TV2    -> 71
PHONO  -> 73
TAPE1  -> 79
TV1    -> 7A
TUNER  -> 7B
CD     -> 7D
```

The queried source IDs preserve the low source-identifying bits:

```text
71 <-> 11
73 <-> 13
79 <-> 19
7A <-> 1A
7B <-> 1B
7D <-> 1D
```

Do not yet assume the receiver literally computes one from the other.

`x13` is also a useful ROOT-vs-`x17` probe:

```text
ROOT:       x13 -> 1?, then 18/14
after x17:  x13 -> no RX / no visible effect
after x07:  x13 works again
```

This probe is established only for these known contexts; it is not yet a universal `whereAmI()` operation.

## Observed control-lock state

Many current bytes reproducibly put the receiver into a state where front-panel controls are locked and ordinary ROOT commands such as `x84` do not select their source.

Observed `LOCK*` bytes in the clean ROOT+CD sweep:

```text
35 36
45 46
55 56
65 66 67
75 76 77
A5 A6
D5 D6
```

`LOCK*` means **observed effect = locked control state**. It does **not** assert that the protocol semantics of these bytes are literally `LOCK`.

Known behavior:

```text
LOCK* byte -> receiver/control panel locked
repeat setter -> remains locked
x15 -> releases the observed locked state
x14 -> does not release it
x07 / x1F / x57 -> do not release it
standby -> wake -> clears it
```

`x13` continues to report source/monitor status while locked. `x15` is therefore labeled `RELEASE*`, not definitively `UNLOCK`; it may be a more general cancel/end/release operation.

The structured distribution of `LOCK*` bytes, plus nearby meaningful commands such as `x85/x86 = TAPE1`, suggests caution about assigning semantic meaning from the lock side effect alone.

## Full ROOT + CD + playing sweep — current logical bytes

This is the clean empirical `00-FF` map from the 2026-10-09 sweep.

Test baseline:

```text
XS context = ROOT
selected input = CD
CD = playing
x13 baseline = 1D,18
```

When a command changed source, the input was returned to CD before continuing. When a command entered the observed lock state, `x15` was used to release it before continuing.

Legend:

- `·` = tested; no observed effect
- `STAT` = `x13` status query
- `REL*` = releases observed lock state; higher-level semantics unknown
- `CTX` = enters the known `x17` subcommand/device context
- `T1`..`T9` = direct CD track selection
- `STOP` = CD stop
- `PAUSE` = non-toggle pause
- `LOCK*` = observed locked-control state; semantics unknown
- `OFF` = receiver power off
- `TUN` = tuner select
- `PHO` = phono select
- `CD` = CD select
- `TP1` = TAPE1 select
- `<<C` / `>>C` = continuous back/forward operation
- `DISC+` = next disc
- `CCRS` = CCRS shown on receiver display
- `P/T` = pause/play toggle
- `RPT` = repeat toggle
- `+10` = CD +10
- `RST` = restart current disc
- `TIME` = time-display cycle
- `|<<C` / `>>|C` = continuous previous/next-track-style operation
- `→D6` = immediate RX `D6`; exact command semantics unknown

| hi\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|---:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| **0** | · | · | · | · | · | · | · | · | · | · | · | · | · | · | · | · |
| **1** | · | · | · | STAT | ·? | REL* | · | CTX | T1 | · | · | · | · | · | · | · |
| **2** | · | STOP | · | · | · | · | · | · | T2 | · | · | · | · | · | · | · |
| **3** | · | STOP | ? | · | · | LOCK* | LOCK* | · | T3 | · | · | · | · | · | · | · |
| **4** | · | · | PAUSE | · | · | LOCK* | LOCK* | · | T4 | · | · | · | ·? | · | · | · |
| **5** | ? | STOP | ? | · | · | LOCK* | LOCK* | · | T5 | · | · | · | · | · | · | · |
| **6** | · | STOP | OFF | · | · | LOCK* | LOCK* | LOCK* | T6 | · | <<C | · | · | · | · | · |
| **7** | · | STOP | · | · | · | LOCK* | LOCK* | LOCK* | T7 | · | >>C | · | · | · | · | · |
| **8** | · | TUN† | TUN | PHO | CD | TP1 | TP1 | · | T8 | · | DISC+ | · | · | · | · | · |
| **9** | · | · | · | · | · | · | · | · | T9 | · | · | · | STOP | · | · | · |
| **A** | · | STOP | · | · | · | LOCK* | LOCK* | · | · | · | · | · | · | · | · | · |
| **B** | · | · | · | · | · | · | CCRS | · | · | · | · | · | P/T | · | · | · |
| **C** | · | · | · | →D6 | · | · | · | · | · | · | · | · | RPT | · | · | · |
| **D** | · | · | · | →D6 | · | LOCK* | LOCK* | · | · | · | +10 | · | · | · | · | · |
| **E** | · | · | · | →D6 | · | RST | RST | · | · | · | TIME | · | |<<C | · | · | · |
| **F** | · | · | · | · | · | · | · | · | · | · | · | · | >>|C | · | · | · |

Notes on a few cells:

- `x14` is marked `·?` because it has produced the familiar `1D,18` pair in earlier tests but has no established visible action; it does not release the lock.
- `x32`, `x50`, `x52`, and the earlier `x4C` observation are not as cleanly documented as the rest of the final sweep and remain `?`/`·?` rather than being silently promoted.
- `x81` visibly selected tuner without an immediate RX in the clean sweep; `x82` selected tuner with RX `7B`.
- `x83` selected PHONO with RX `73`.
- `x84` selected CD with RX `7D`.
- `x85` and `x86` selected TAPE1 with RX `79`.
- `x8A` is **NEXT DISC**. Earlier notes that called it previous disc were wrong.
- `xCC` is **REPEAT toggle** despite no RX.
- `xE5` and `xE6` restart the current disc from the beginning.
- `xEA` cycles CD time display: single/total and played/remaining modes were observed.
- `xEC` is `|<<` continuous behavior.
- `xFC` is `>>|` continuous behavior.

### Direct-track family

Clean source-gated family:

```text
18 -> TRACK 1
28 -> TRACK 2
38 -> TRACK 3
48 -> TRACK 4
58 -> TRACK 5
68 -> TRACK 6
78 -> TRACK 7
88 -> TRACK 8
98 -> TRACK 9
```

These commands are active with CD selected and can be silent after changing away from CD.

### STOP family and RX `D6`

Observed STOP commands include:

```text
21
31
51
61
71
A1
9C
```

`71` produced delayed RX `D6` after ~320 ms.  
`9C` produced delayed RX `D6` after ~330 ms.

Separately:

```text
C3 -> D6 in ~3.8 ms
D3 -> D6 in ~3.6 ms
E3 -> D6 in ~4.5 ms
```

A second `B6` invocation also produced immediate `D6` while CCRS remained active.

Therefore `D6` must **not** be labeled simply `STOP ACK`. It behaves more like a changer status/event report whose exact meaning is still unresolved.

### Changer state/event reports seen so far

Observed reports:

```text
D6  changer status/event; exact meaning unresolved
D8  first CCRS activation
DA  pause event/state
DE  play/resume event/state
7D  receiver CD source broadcast/select following play/resume
35  observed once after EC continuous operation; not yet assigned
```

Known physical changer-originated behavior from passive observation also includes:

```text
PLAY from STOP:   changer -> DE, then receiver -> 7D
PAUSE:            changer -> D2
PLAY from PAUSE:  changer -> DE, then receiver -> 7D
STOP:             changer -> D6
```

The `DA` versus passive `D2` pause discrepancy remains unresolved and should be retested rather than normalized away.

### CCRS

```text
xB6 -> CCRS visibly appears on receiver display
first observed response: D8
second xB6: CCRS remains active; not a toggle; response D6
```

Thus `xB6` is a set/start-like CCRS command, not a toggle.

### Pause/play

```text
playing:
xBC -> PAUSE
        RX DA

paused:
xBC -> PLAY/resume
        RX DE
        ~107 ms later RX 7D
```

`x42` is a separate non-toggle PAUSE command.

## `x17` context — observed tuner-like command map

The following table uses **legacy byte representation**, because this mapping originated in the older capture/decoder convention. Do not mix these values with the current ROOT matrix without applying `current = reverse8(~legacy)`.

| Legacy byte | Observed action |
|---|---|
| `D0` | TUNE UP |
| `D1` | PRESET CH7 |
| `D2` | BAND: AM -> FM1 -> FM2 |
| `D3` | PRESET CH3 |
| `D4` | no visible effect |
| `D5` | PRESET CH5 |
| `D6` | PRESET CH9 |
| `D7` | PRESET CH1 |
| `D8` | TUNE DOWN |
| `D9` | PRESET CH6 |
| `DA` | no visible effect |
| `DB` | PRESET CH2 |
| `DC` | no visible effect |
| `DD` | PRESET CH4 |
| `DE` | PRESET CH8 |
| `DF` | PRESET CH10 |

Other legacy observations in this context:

```text
51 -> DIRECT FREQUENCY ENTRY
57 -> TUNER PRESET SCAN START/STOP
5B -> DIRECT CHANNEL ENTRY
```

Numeric/direct-entry behavior was explored but the submode transitions were mixed; remap cleanly before treating the numeric table as final.

Current-byte spot checks establish the context transition itself:

```text
ROOT:
x84 -> CD, RX 7D

x17:
x13 -> no RX
x84 -> tuner + CH8

x07:
returns interpretation to ROOT
```

The higher-level semantics of `x17` remain open. It may be a tuner-specific context, or it may be a more generic component/subcommand context whose observed map happens to address tuner operations.

## Legacy receiver observations

The following values are preserved only as **legacy-representation historical observations**. They must not be mixed into the current logical ROOT matrix:

```text
3E PHONO
5E TAPE1
60 VOLUME UP / hold
63 POWER OFF
68 VOLUME DOWN / hold
69 CD
6D all display segments
B5 POWER ON
B9 POWER OFF
BE TUNER
D0 tune/scan up
D1 preset CH7
DE CD
E1 VIDEO1
E3 TAPE1
EC display toggle
ED monitor toggle
EE VIDEO2
EF PHONO
```

Several historical observations are clearly state/context dependent when compared with the clean current ROOT+CD sweep. Preserve them as evidence; do not overwrite the clean map with them.

## Full-sweep conclusion

The controlled `ROOT + CD + playing` one-byte sweep covered all `00-FF`.

Strong conclusions:

1. The ROOT command space contains many direct CD controls; a separate CD context is not required for basic transport, track, disc, repeat, restart, or display-time operations.
2. No second obvious one-byte context-enter command was found.
3. `x17` remains the only cleanly demonstrated context transition, with `x07` returning to ROOT.
4. The meaning of `x17` should remain open: **tuner-like context observed; generic device/component context still possible**.
5. The repeated `LOCK*` effects are real, but their protocol semantics are not established.
6. RX bytes such as `D6/D8/DA/DE` should be treated as changer status/event reports until their meanings are independently established.

## Next targeted experiments

1. Re-test `x17` without assuming it means TUNER:
   - enter with `x17`
   - verify `x13` is silent
   - send known ROOT CD commands (`BC`, `9C`, `CC`, `EA`, `EC`, `FC`)
   - observe whether they retain CD meaning, acquire another meaning, or are ignored
   - return with `x07`
2. Determine whether `x17` is a generic component/subcommand context and, if so, how a target component is selected.
3. Map the `LOCK*` state more carefully without calling the setter bytes `LOCK` semantically.
4. Resolve the `D2` vs `DA` PAUSE-report discrepancy.
5. Characterize `D6`, including immediate versus delayed occurrences.
6. Cleanly remap direct-frequency/direct-channel entry using one byte convention only.
7. Continue preserving raw logs for any asynchronous changer reports rather than automatically attributing them to the immediately preceding TX byte.
