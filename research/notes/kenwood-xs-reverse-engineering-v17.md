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
- `STAT` = `x13` status query; `x14` also returned the same status pair in ROOT+CD
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
| **1** | · | · | · | STAT | STAT | REL* | · | CTX | T1 | · | · | · | · | · | · | · |
| **2** | · | STOP | · | · | · | · | · | · | T2 | · | · | · | · | · | · | · |
| **3** | · | STOP | · | · | · | LOCK* | LOCK* | · | T3 | · | · | · | · | · | · | · |
| **4** | · | · | PAUSE | · | · | LOCK* | LOCK* | · | T4 | · | · | · | · | · | · | · |
| **5** | · | STOP | · | · | · | LOCK* | LOCK* | · | T5 | · | · | · | · | · | · | · |
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

- Follow-up ROOT+CD tests (2026-10-09) closed five uncertain cells: `x14` returned `1D,18` (~0.7 ms, ~109.8 ms), matching `x13`; `x32`, `x4C`, `x50`, and `x52` had no RX and no reported visible effect. After each, `x13` still returned `1D,18`. `x14` is thus `STAT` **in ROOT+CD**, not yet proven globally equivalent to `x13`.
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

## Follow-up: `x17` context, selected confirmed observations (2026-10-09)

This section predates the targeted sweep documented below. `x13` remains silent in the observed alternative context; `x07` restores ROOT and status responses.

Confirmed TUNER controls in `x17` (current logical TX bytes):

| TX | Observed behavior |
|---|---|
| `x04`, `x14`–`x94` | Preset CH10, CH1–CH9 respectively |
| `xB4` | Band switch |
| `xE4`, `xF4` | Tune down / up |
| `x05` | Auto tune toggle |
| `x15` | Preset scan |
| `x25` | Memory/channel direct-entry behavior; exact semantics uncertain |
| `x75` | Direct frequency entry |

CD transport also continues after `x17`:

| TX | Observed CD behavior | Other observations |
|---|---|---|
| `x18` | Track 1 repeatedly | `x13` remains silent |
| `x42` | Pause (non-toggle) | `x13` remains silent |
| `xBC` | Play/pause toggle | RX `DE` (play), `DA` (pause), ~3.5–5.2 ms; TUNER input selection observed |
| `xCC` | Repeat toggle | No RX |
| `x8A` | Next disc | Input switches to TUNER, reproduced after manually restoring CD |
| `xE5`, `xE6` | Play/restart | Input switches to TUNER; **not** evidence of CH8 selection |
| `xB6` | CCRS | RX `D8` at +3.6 ms; subsequent tuner selection observed, preset causality uncertain |

Direct A/B for `x8A`: after `x07`, `x13` returned `1D,18`, NEXT DISC worked and input remained CD; after `x17`, NEXT DISC worked but input switched to TUNER and `x13` remained silent. Without changer connected, `x8A` produced no visible effect in the tested setup. These facts do not establish which device generates the source-selection side effect. A brief impression that changer front-panel buttons also triggered TUNER selection was **withdrawn as unconfirmed**.

For `x?5`, other tested codes had no observed reaction. For `x?6`, `x06`–`xA6`, `xC6`, `xD6`, `xF6` had no observed reaction in the tested context. No-RX alone is not proof of no action.

## Completion status and remaining uncertainties

The controlled ROOT+CD single-byte matrix is now populated, including the five follow-up cells. The observed LOCK* behavior is already sufficiently characterized for this command map: setter family, persistent lock, `x15` release, `x13` status still responding, and blocked `x84` source selection. The **semantic purpose** of LOCK* remains unknown; it should not be relabeled as a proven literal LOCK command.

Remaining questions are interpretive, not blockers for the primary command map: exact meaning of `x17`, the mechanism of input selection during changer actions, and semantics of certain changer event/status bytes (`D6`, `D8`, `DA`, `DE`). No further broad sweep is currently planned.

## `x17` context — targeted sweep and comparative map (2026-10-09)

This table uses **current logical TX bytes**, matching the ROOT table above. Each cell is the observed `x17` action, not a decoded RX byte. `·` means **no RX and no separately confirmed physical action in the available observations**, not a universal NOP. Several known actions have no RX. Commands with known effects were sometimes excluded from the targeted pass and are included from earlier tests. `x13` is silent throughout the `x17` pass.

| hi\lo | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | A | B | C | D | E | F |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **0** | · | · | · | · | CH10 | AUTO | · | ROOT | PHONO | HOLD | · | · | · | · | · | · |
| **1** | · | · | · | · | CH1 | SCAN | · | CTX | T1 | · | · | · | · | · | · | · |
| **2** | · | · | · | · | CH2 | MEM? | · | · | · | · | · | · | · | · | · | · |
| **3** | · | · | · | · | CH3 | · | · | · | · | OFF | · | · | · | · | · | · |
| **4** | · | · | PAUSE | · | CH4 | · | · | · | · | DISP | · | · | · | · | · | · |
| **5** | · | · | · | · | CH5 | · | · | · | · | · | · | · | · | · | · | · |
| **6** | · | · | · | · | CH6 | · | · | · | · | CD DIR | · | · | · | · | · | · |
| **7** | · | · | · | · | CH7 | FREQ | · | · | · | · | · | · | · | · | · | · |
| **8** | · | · | · | · | CH8 | · | · | · | · | · | DISC+ | · | · | · | · | · |
| **9** | · | · | · | · | CH9 | · | · | · | · | · | · | · | · | · | · | · |
| **A** | · | · | · | · | · | · | · | · | · | SURR | · | · | · | · | · | · |
| **B** | · | · | · | · | BAND | · | CCRS | · | · | · | · | · | PLAY/PAUSE | · | · | · |
| **C** | · | · | · | · | · | · | · | · | SLEEP | · | · | · | REPEAT | · | · | · |
| **D** | · | · | · | · | · | · | · | · | MUTE | · | · | · | · | · | · | · |
| **E** | · | · | · | · | TUNE− | RESTART | RESTART | · | · | VOL− | · | · | · | · | · | · |
| **F** | · | · | · | · | TUNE+ | · | · | · | · | VOL+ | · | · | · | · | · | · |

**Additional known behavior:** In `x17`, changer transport continues to work for several commands (including `x8A` next disc and `xBC` play/pause); this table does **not** claim all changer commands were re-tested in the targeted pass. `x?5` and `x?6` had earlier partial family tests. `x35/x36/x45/x46/x55/x56/x65/x66/x67/x75/x76/x77/xA5/xA6/xD5/xD6` were checked for ROOT-style control lock: **no lock and no RX** in `x17`. Note that `x75` is nevertheless a known tuner direct-frequency-entry command in `x17`; "no lock" is not "no action".

**Receiver controls newly observed in `x17`:** `x08` PHONO input; `xC8` SLEEP toggle; `xD8` MUTE toggle; `x39` POWER OFF with RX legacy `B9` at +10.2 ms; `x49` all display segments lit (next `x13` restored normal display, despite no RX while in `x17`); `x69` CD DIRECT toggle; `xA9` SURROUND toggle; `xE9` VOL− continuous; `xF9` VOL+ continuous; `x09` stops continuous volume change. The latter three constitute START DOWN / START UP / STOP, rather than individual volume steps.

**Context caveat:** After `x39` POWER OFF, an `x13` query again returned `1D,18` (+1.2/+110.3 ms), consistent with return to ROOT. The subsequent explicit `x17` restored silent `x13`; `x49` then lit all display segments. POWER OFF resetting context is a strong working interpretation, not yet an independently isolated guarantee.

**Sweep coverage:** full `x?0`, `x?B`, `x?D`, `x?E`, `x?F` families; targeted `x?1`, `x?2`, `x?3`, `x?4`, `x?7`, `x?8`, `x?9`, `x?A`, `x?C`; previously explored `x?5` and `x?6`; LOCK family tested explicitly. `x0A` and `xAA` were later checked with no RX. Final exit: `x07` no RX; `x13` returned `1D` at +0.9 ms and `18` at +109.9 ms. Thus ROOT return is directly verified.

**Comparing ROOT and `x17`:** Use the ROOT 16×16 table in this document alongside the `x17` table above. They share the same logical-byte row/column indexing. The most important changed interpretations include `x84` (ROOT CD input; `x17` tuner CH8), `x75` (ROOT LOCK*; `x17` direct frequency entry), `x13` (ROOT STATUS; `x17` no RX), and `x15` (ROOT RELEASE*; `x17` tuner preset scan).
