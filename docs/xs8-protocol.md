# Kenwood XS8 interface: working engineering specification

**Scope.** This documents the observed Kenwood KR-V6070 receiver and connected CD changer, and the original Arduino Micro transmitter. The production transmitter is now the Raspberry Pi GPIO `xs8-service` ([architecture](architecture.md), [project status](project-status.md)). Electrical details below that name the Arduino describe that original setup. It is an implementation reference for this setup, not a complete or universal XS8 protocol definition. “Observed” means either an archived waveform or a reported bench response; where only a note survives, that limit is stated. See the [command registry](../research/COMMAND_REGISTRY.md) and [research history](../research/RESEARCH_HISTORY.md) for the full evidence and conflicting observations.

## Electrical interface

| TRS contact | Observed line | Arduino Micro connection in the working setup |
|---|---|---|
| Sleeve | Ground | Common ground |
| Tip | BUSY / control envelope | D3 TX through 2.2 kΩ; A5 RX |
| Ring | DATA | D2 TX through 2.2 kΩ; A0 RX |

Both lines idle LOW in the tested setup. The working transmitter drives HIGH using `OUTPUT HIGH` and produces LOW by changing its TX pin to `INPUT` (high impedance); it releases both pins after a frame. It does not actively drive LOW. Its BUSY receiver uses an ADC threshold of 500 and requires BUSY LOW for 500 µs before starting a frame, with a 100 ms bus-free timeout. These are implementation settings, not measured electrical limits or proof of compatibility with other XS equipment. [Electrical observations](../research/notes/kenwood-xs-reverse-engineering-reviewed.md), [current transmitter](../xs8-bridge/xs8-bridge.ino)

## Frame timing and byte encoding

The archived [edge capture](../research/captures/xs-session2.csv) and live experiments support this nominal frame shape:

1. Assert BUSY HIGH.
2. About **1,150 µs** later, assert DATA HIGH for a **9,800 µs** start pulse.
3. Send eight data bits. Each bit is DATA LOW for about **4,900 or 9,800 µs**, then DATA HIGH for about **4,900 µs**.
4. In the current firmware, release DATA, wait **750 µs**, then release BUSY. This tail is an experimental working choice and differs from the originally observed Kenwood tail.

The current **logical TX** convention sends bit 0 first (LSB first): a short LOW is logical `0`; a long LOW is logical `1`. The transmitter loops over bits `0..7`, using 4,900 µs for `0` and 9,800 µs for `1`. [Timing and tail](../xs8-bridge/xs8-bridge.ino), [research timing summary](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)

## TX and RX byte notation

All `xNN` **TX** values below use the current logical convention. Earlier TX sketches printed an MSB-first **legacy TX** byte with short LOW = `1` and long LOW = `0`. For the same waveform, `logical = reverse8(~legacy)`; for example, legacy TX `DE` is logical TX `x84`, and legacy `60/68` are logical `xF9/xE9`. [Mapper conversion](../research/sketches/xs-tx-test1.ino)

**RX** byte labels are retained as decoded in their source. The later TX/RX test sketch deliberately decodes RX MSB first with short LOW = `1` while it sends logical TX bytes, so `TX x84 → RX 7D` mixes two named conventions. The early passive note prints some front-panel source RX labels that are bitwise complements of later labels (early CD `82`, later CD `7D`). A decoder polarity change is plausible, but its exact historical cause has not been verified. Do not silently transform RX bytes. [Later RX decoder](../research/sketches/xs-tx-test2.ino), [early passive source table](../research/notes/kenwood-xs8-sniffing-notes.md), [later source broadcasts](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)

## Context and observed state dependence

In the tested ROOT state, logical TX `x13` elicited two receiver RX frames: a source-dependent first byte and a monitor-dependent second byte about 110–120 ms later. After TX `x17`, `x13` was silent and TX `x84`, which selected CD in ROOT, selected tuner preset CH8. TX `x07` restored the tested ROOT interpretation and `x13` responses; logical TX `x1F` did not. The higher-level meaning of the `x17` context is unknown: tuner controls are prominent, yet selected CD transport commands continued to work there. Audio input and command context must be tracked separately. [Context probes](../research/notes/kenwood-xs-reverse-engineering-reviewed.md), [targeted `x17` tests](../research/notes/kenwood-xs-reverse-engineering-v18.md)

On 2026-10-11, operator-reported physical `QUERY_STATUS` trials checked all six documented source associations and both TAPE2 MONITOR values on the KR-V6070. The first later-decoder legacy RX was `11/13/19/1A/1B/1D` for VIDEO2/PHONO/TAPE1/VIDEO1/TUNER/CD; the second was `18` (OFF, waveform `E7`) or `14` (ON, waveform `D7`). These are bus observations associated in time with logical TX `x13`, while the source and MONITOR labels are interpretations checked against receiver state. The new raw edge traces are not archived. CD DIRECT and SURROUND changes produced no passive traffic in the tested toggles and did not change these two fields. This does not establish their unreadability by other commands. [October 11 physical report](../research/notes/xs8-readback-2026-10-11.md)

The same TX value can have different observed effects by context. For example, `x15` released an observed control-lock state in ROOT but started tuner preset scan after `x17`; `x75` produced a lock effect in the ROOT+CD sweep but entered direct frequency mode after `x17`. These are observations, not proof of formal command names or equivalence across states. [Cross-context observations](../research/notes/kenwood-xs-reverse-engineering-v18.md)

### Research context lifecycle: evidence and enablement boundary

The supported lifecycle is narrower than a general recovery primitive:

| Stage | Evidence | Limit |
|---|---|---|
| Enter from the tested ROOT state | Logical `x17` changed subsequent `x84` interpretation and made `x13` silent. | No positive context-entry acknowledgment or additional handshake is documented. Silence cannot distinguish entry from a missing response. [Controlled probes](../research/notes/kenwood-xs-reverse-engineering-reviewed.md#L413-L425) |
| Return from the tested entered states | Logical `x07` restored ROOT interpretation and status responses; the targeted sweep's final `x13` returned legacy RX `1D` and `18`. | These are reported bench observations, not a universal guarantee after every possible context command. [Targeted exit](../research/notes/kenwood-xs-reverse-engineering-v18.md#L553-L562) |
| End a known volume hold | `x09` before `x07`; the working volume controller retains STOP and context obligations across failed TX. | This is a volume-specific termination sequence. [Current state machine](../volume_control.cpp#L116-L195) |
| Recover an arbitrary latched operation | No general sequence established. `x07` did not release the observed ROOT front-panel lock; `x15` did, but `x15` starts preset scan after `x17`. | Context exit, volume STOP, CD RELEASE and lock release cannot be substituted for one another. [Lock recovery observations](../research/notes/kenwood-xs-reverse-engineering-reviewed.md#L195-L206) |

The production entry mechanism comprises a clean service state, BUSY-free checks, a normal logical `x17` frame, observed waveform verification, and conservative `context_may_be_open` tracking before TX. It has no receiver acknowledgment step. Volume and MUTE then execute their known commands and cleanup; the existence of these working transactions does not validate an arbitrary-byte research session. [Entry and MUTE state handling](../volume_control.cpp#L160-L195), [MUTE cleanup](../volume_control.cpp#L239-L261), [BUSY checking](../xs8_gpio.c#L143-L162), [service TX evidence](../xs8_service.cpp#L54-L93)

Context entry outside the production volume and MUTE transactions is available only through the attended, default-off [interactive CONTEXT research session](xs8-research-repl.md#interactive-context-research-session-attended-default-off). It is not a validated control path. The archive establishes normal entry/exit for tested states, but not an automatic safe recovery profile for unknown VIDEO 1 / VIDEO 2 candidates, a positive general context witness, or recovery after an interrupted arbitrary command. The existing `x17 → x09 → x07` startup recovery is explicitly experimental in the source and is not a substitute for that missing evidence. [Experimental startup recovery](../volume_control.cpp#L76-L113)

An [attended, non-transmitting MUTE preparation](xs8-research-repl.md#attended-mute-experiment-preparation-no-tx) now reserves the service and records passive bus observations for the characterized `x17 → xD8 → x07` profile. RUN refuses TX. The smallest enablement prerequisite is one archived use of the existing production MUTE transaction demonstrating three distinct postconditions: restored ROOT interpretation, working front-panel controls, and working normal receiver control. A status reply alone is insufficient because the documented control lock still allowed `x13` responses. Preparation cancellation/expiry sends no cleanup command and establishes no physical recovery claim.

The [proposed research session design](xs8-research-repl.md#original-context-session-design-notes) requires a reviewed command allowlist and command-specific recovery evidence before any entry gate is enabled. `TX_OK` and waveform `MATCH` establish electrical transmission evidence; ROOT status response fields establish associated observations. None proves that all receiver/changer activity has terminated.

## Working control sequences

These are **logical TX bytes in exact order**. Each byte is a separate XS8 frame; the current firmware waits for BUSY to be free before each frame. The volume and mute actions were reported from hardware tests, and the sequences are implemented by the working bridge. No archived end-to-end CEC/serial/XS capture independently verifies every complete bridge transaction. [Observed controls](../research/notes/kenwood-xs-reverse-engineering-v18.md), [bridge implementation](../xs8-bridge/xs8-bridge.ino)

| Operation | Exact TX order | Observed behavior and required handling |
|---|---|---|
| Start volume up | `x17 → xF9` | Enter the observed alternate context, then begin **continuous** volume increase. |
| Start volume down | `x17 → xE9` | Enter the context, then begin **continuous** volume decrease. |
| Stop active volume and return to ROOT | `x09 → x07` | **Send STOP before context exit.** `x09` stops the continuous change; `x07` restores ROOT. Do not reverse or omit this order. |
| Change volume direction | `x09 → x07 → x17 → xE9` or `x09 → x07 → x17 → xF9` | Finish the prior hold before starting the other direction. |
| Mute toggle | `x17 → xD8 → x07` | Toggle MUTE in the observed context, then return to ROOT. If volume is active, first send `x09 → x07`. Physically confirmed as a **toggle** in both directions (mute, and unmute while muted) on 2026-10-12. |

### Context source selection: VIDEO1 / VIDEO2 (validated 2026-10-12, now implemented)

| Operation | Exact TX order | Evidence and limits |
|---|---|---|
| Select VIDEO1 | `x17 → x78 → x07` | Physically observed (operator). ROOT `x13` afterwards reported `source_inferred=TV1` (the label then in use; now `VIDEO1`). |
| Select VIDEO2 | `x17 → x88 → x07` | Physically observed (operator). ROOT `x13` afterwards reported `source_inferred=TV2` (now `VIDEO2`). |

Both were sent through the attended interactive CONTEXT session with human-paced gaps. Each exit `x07` was `TX_OK`/`MATCH` with one attempt. After each trial the front panel worked and ROOT `x84` selected CD, not tuner CH8. In ROOT, `x78`/`x88` are CD tracks 7/8, and ROOT `x78` with TUNER selected did not select VIDEO1, so **the `x17` entry is required**. `TX_OK`/`MATCH` and the inferred query labels are not the physical evidence; the operator's observation is. These were research results when recorded. They are **now production commands**: `SELECT_SOURCE VIDEO1|VIDEO2` sends exactly these sequences as one atomic service transaction ([receiver controls](receiver-controls-api.md), [source selection](source-selection.md)). The same CONTEXT mechanism sends TAPE2 MONITOR `x17 → x48 → x07`. The operator reported it physically verified on 2026-10-15, with readback MONITOR ON as waveform `0xD7` and OFF as `0xE7`. [Trial record](../research/notes/xs8-context-video-mute-2026-10-12.md)

The firmware tracks an outstanding volume stop and retries STOP/ROOT transmission after BUSY failures. It uses a 1,200 ms heartbeat watchdog, but a permanently busy bus can prevent a STOP frame. A host should explicitly request stop on key release and shutdown; the watchdog is a fallback. [Failure handling](../xs8-bridge/xs8-bridge.ino)

**CD STOP is different.** The current application command is ROOT logical TX **`xA1`**: `CD_STOP` sends it, and it has been physically verified on the KR-V6070 (operator report, 2026-10-14). ROOT `x21` also stopped CD through the research REPL ([CD transport](cd-transport-api.md#single-frame-keys-play-playpause-stop-time-display-repeat-and-disc-skip)). **Historical finding:** in ROOT with a connected, active changer, logical TX `x9C` was observed to stop playback; RX `D6` followed after about 330 ms. `x9C` is no longer sent by `CD_STOP`: a VIDEO1/VIDEO2 selection after a `x9C` STOP was reported to provoke an unwanted changer restart. `x09` is the observed continuous-volume STOP in the `x17` context, not an interchangeable CD STOP. Other ROOT STOP-like bytes were reported in the sweep. RX `D6` also appeared after other commands, so its semantics are unresolved. [CD STOP observations](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)

A later operator-reported physical ROOT trial found `x52` (legacy TX `B5`) powered on a standby KR-V6070; its connected changer powered through the receiver's switched AC outlet. ROOT `x41` (legacy TX `7D`) lit the CD PLAY indicator. A service-side `x41 → x9C` trial used a requested 500 ms TX-end-to-next-TX-start gap (approximately 506.7 ms observed) and ended in CD STOP. These results are state-bound physical reports without deposited raw traces. The earlier CD-playing sweep saw no visible action from `x41`, and an already-on follow-up saw none from `x52`; preserve those baseline differences. The PLAY-to-STOP result does not establish `x9C` as a generic key release. [Physical report](../research/notes/xs8-root-validation-2026-10-12.md), [earlier ROOT matrix](../research/notes/kenwood-xs-reverse-engineering-v18.md#L250-L276)

### CD transport PRESS and RELEASE in ROOT

October 12 attended trials physically validated logical TX `x72` (legacy TX `B1`) as a common **CD transport KEY UP/RELEASE** after four ROOT-context key-down operations. Historical ROOT matrix labels `x6A` as continuous backward (`<<C`) and `x7A` as continuous forward (`>>C`), consistent with the reported SEEK directions. All bytes in this table are transmitted directly in ROOT, with no `x17`. The physical outcomes were operator-observed; per-trial raw edges and service replies were not deposited. `TX_OK` or waveform `MATCH` alone would not prove receiver execution. [Physical trial report](../research/notes/xs8-cd-key-release-2026-10-12.md), [historical directions](../research/notes/kenwood-xs-reverse-engineering-v18.md#L230-L269)

| Operation | Logical TX sequence (legacy TX) | Reported physical outcome |
|---|---|---|
| TRACK NEXT press/release | `xFC → x72` (`C0 → B1`) | One track forward near a 100 ms requested gap; two tracks near 500 ms. |
| TRACK PREVIOUS press/release | `xEC → x72` (`C8 → B1`) | Restarted the current track; `xEC → x72 → xEC → x72` moved to the previous track (`id=34`). |
| SEEK BACKWARD press/release | `x6A → x72` (`A9 → B1`) | Seek started and terminated (`id=35`). |
| SEEK FORWARD press/release | `x7A → x72` (`A1 → B1`) | Seek started and terminated, repeated twice (`id=36–37`). |
| CD STOP/CANCEL after NEXT | `xFC → x9C` (`C0 → C6`) | Continuing skip cancelled **and playback stopped**. `x9C` is not equivalent to `x72` KEY UP. |

The successful PRESS/RELEASE pairs requested **100 ms** from the end of one TX to the beginning of the next; actual gaps were reported collectively as approximately **104–109 ms**. These values are observations, not a hard maximum or a guaranteed single-track threshold. A queued sequence can abort before RELEASE, and an unobserved or malformed release frame can leave an operation active. The older ROOT+CD+playing sweep recorded **no visible effect** for `x72` without an active TRACK/SEEK operation; that state-dependent negative result remains part of the record. `x72` is established only for these four tested CD operations, not as a universal XS8 release. Volume `x09`, context exit `x07`, front-panel-lock release `x15`, and CD STOP `x9C` retain their distinct observed roles. [Older baseline](../research/notes/kenwood-xs-reverse-engineering-v18.md#L250-L269), [paired trials](../research/notes/xs8-cd-key-release-2026-10-12.md)

## Conservative application subset

Use these only with the stated preconditions and an application-maintained context state. Their inclusion means the action was observed in this setup; it does **not** claim universal safety across receiver states. A lost frame or unknown context calls for a verified recovery procedure, not an assumed NOP.

| Operation | Logical TX | Tested precondition / limit |
|---|---|---|
| Query source and monitor response | `x13` | ROOT; response interpretation established for the tested receiver sources and monitor states. Silent after `x17`. |
| Select CD input | `x84` | ROOT; RX `7D` observed. After `x17`, the same byte selected tuner CH8. |
| Select TUNER or PHONO | `x82` or `x83` | ROOT; RX `7B` or `73` observed in the controlled sweep. |
| Select TAPE1 | `x85` or `x86` | ROOT; RX `79` observed. The distinction between the two TX bytes is unknown. |
| Stop CD playback | `xA1` | ROOT, CD selected and changer active; current `CD_STOP` command, operator-verified. Historical `x9C` also stopped CD but is no longer used (unwanted changer restart after a later VIDEO1/VIDEO2 selection). Delayed RX `D6` is not a proven acknowledgment. |
| Volume and mute | Sequences above | Receiver on; track whether `x17` is open, guarantee ordered `x09 → x07` after volume start, and handle BUSY failure. |

This subset deliberately excludes the ROOT lock-producing family and bytes whose only evidence is silence, a library name, or an uncontrolled historical state. [Controlled ROOT map and lock family](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)

The standalone [CD transport socket API](cd-transport-api.md) owns discrete `xFC/xEC → x72` taps and explicit TRACK/SEEK holds with a release obligation and renewable client lease. Raw CD TRACK/SEEK key-down bytes are **not** added to this conservative subset: a lost release can leave the changer operating, and neither TX completion nor waveform match proves physical termination.

## Hazards and limits of evidence

- Continuous volume can persist after one start frame. A lost `x09` can leave the receiver changing volume; a BUSY line that never clears defeats the bridge watchdog’s attempt to transmit STOP.
- Continuous CD TRACK/SEEK can persist after key down. The observed `x72` pairs terminated the four tested operations, but TX completion and an RX waveform match do not guarantee physical termination after a lost or malformed release; `x9C` stops playback as well as skipping in the reported `xFC → x9C` trial. [CD release report](../research/notes/xs8-cd-key-release-2026-10-12.md)
- In ROOT, `x35 x36 x45 x46 x55 x56 x65 x66 x67 x75 x76 x77 xA5 xA6 xD5 xD6` produced an observed persistent front-panel control lock. Their protocol semantics are unknown. They are **not** in the application subset. [Lock tests](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)
- RX silence or no recorded visible action is not evidence that a byte is a NOP. The broad ROOT sweep was performed with CD selected and playing; the `x17` sweep was targeted, and other states remain incompletely tested. [Sweep conditions](../research/notes/kenwood-xs-reverse-engineering-reviewed.md), [targeted-sweep caveat](../research/notes/kenwood-xs-reverse-engineering-v18.md)
- Changer RX `D6`, `D8`, `DA`, and `DE` have observed associations, not established field meanings. Passive PAUSE was labeled RX `D2`, while active pause yielded RX `DA`; this contradiction remains open. [Changer reports](../research/notes/kenwood-xs-reverse-engineering-reviewed.md), [passive pause](../research/notes/kenwood-xs8-sniffing-notes.md)
- The CSV captures support waveform timing, and the retained serial scan shows a state change at legacy TX `17`; the later full command matrices primarily survive as written bench observations rather than raw, labeled traces. The bridge source records the implemented behavior, but no archived end-to-end CEC trace proves every transaction. [Partial scan](../research/captures/kenwood-xs-scan.txt), [evidence inventory](../research/RESEARCH_HISTORY.md)
