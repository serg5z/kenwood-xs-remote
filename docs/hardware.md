# Kenwood XS Remote hardware

This document describes two **experimentally validated hardware interfaces** for the Kenwood KR-V6070: the original Arduino Micro bridge and the Raspberry Pi B+ GPIO interface. Neither is a certified interface for other Kenwood SYSTEM CONTROL devices. The [XS8 protocol reference](xs8-protocol.md) covers frame encoding and commands, and the [architecture](architecture.md) covers the software path.

**Integration status:** The **Raspberry Pi GPIO interface is the production path.** `xs8-service` owns GPIO4/22 (TX) and GPIO27/23 (RX sense) and serves both the CEC client and the Web Remote ([project status](project-status.md)). The Raspberry Pi → USB → Arduino Micro bridge was the original CEC path. It is now legacy and kept for reference only.

## Components and connections

| Component | Role in this project |
|---|---|
| Raspberry Pi B+ | Drives XS8 directly through GPIO and `libgpiod` in [`xs8-service`](../xs8_service.cpp). Also runs the CEC client and Web Gateway. Originally it ran a `libCEC` host that sent one-character commands to the Arduino over `/dev/ttyACM0`. [Pi transmitter](../xs8_send.c) |
| Arduino Micro, 5 V setup (legacy) | Converted USB serial commands into timed XS8 frames in the original CEC bridge. [Working firmware](../xs8-bridge/xs8-bridge.ino), [bench summary](../research/notes/kenwood-xs-reverse-engineering-reviewed.md) |
| Kenwood KR-V6070 | Receiver whose SYSTEM CONTROL/XS connector and responses were investigated. Some research tests also used a connected Kenwood CD player/changer. [Passive observations](../research/notes/kenwood-xs8-sniffing-notes.md) |

The tested XS8 connector uses a **TRS** plug:

| Plug contact | XS signal |
|---|---|
| Sleeve | GND |
| Tip | `S.BUSY` / BUSY |
| Ring | `S.DATA` / DATA |

## Arduino Micro XS8 interface (original, validated, now legacy)

| Plug contact | XS signal | Arduino-side connection |
|---|---|---|
| Sleeve | GND | Common ground |
| Tip | `S.BUSY` / BUSY | D3 through **2.2 kΩ** to BUSY node; A5 senses the same node |
| Ring | `S.DATA` / DATA | D2 through **2.2 kΩ** to DATA node; A0 senses the same node |

```text
Raspberry Pi B+ ── USB ── Arduino Micro
                         D2 ── 2.2 kΩ ──┬── TRS ring: S.DATA
                         A0 ────────────┘
                         D3 ── 2.2 kΩ ──┬── TRS tip: S.BUSY
                         A5 ────────────┘
                         GND ────────────── TRS sleeve: GND
                                            Kenwood KR-V6070
```

The 2.2 kΩ resistors are **in the TX paths**. A0 and A5 sense the XS-side nodes; they do not replace the series resistors. This Arduino configuration does **not** use Pi GPIO-to-XS wiring. The separate Pi GPIO interface is described below. [Arduino pin declarations](../xs8-bridge/xs8-bridge.ino), [physical mapping](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)

## Raspberry Pi B+ GPIO XS8 interface (production)

A second implementation was tested on the **Kenwood KR-V6070** using a **Raspberry Pi Model B+ Rev 1.2** and external transistor-based 5 V TX stages. The program [`xs8_send.c`](../xs8_send.c) uses `libgpiod` to generate XS8 timing directly on the Pi. The same GPIO interface is now driven by `xs8-service`, the production path for the CEC client and the Web Remote. `xs8_send` remains a standalone command-line transmitter.

| Pi BCM GPIO | Direction / function | Electrical connection |
|---|---|---|
| GPIO4 | DATA TX control | New 2N3904/2N3906 driver → **2.2 kΩ** series resistor → XS8 ring |
| GPIO22 | BUSY TX control | Separate 2N3904/2N3906 driver → **2.2 kΩ** series resistor → XS8 tip |
| GPIO27 | DATA RX sense | XS8 ring via **10 kΩ / 10 kΩ** voltage divider |
| GPIO23 | BUSY RX sense | XS8 tip via **10 kΩ / 10 kΩ** voltage divider |
| GND | Common reference | XS8 sleeve |

Both TX drivers are powered from 5 V. **Pi GPIO pins are 3.3 V signals and must not be connected directly to the 5 V XS8 lines.** The driver control GPIO HIGH asserts the corresponding XS8 line HIGH; control GPIO LOW releases the line. The intended idle state is **GPIO4 LOW and GPIO22 LOW**, with the XS8 bus observed LOW when idle. The RX dividers are sense connections, not output drivers.

```text
Raspberry Pi B+                       Kenwood KR-V6070 XS8
GPIO4  ── DATA 5 V transistor TX ── 2.2 kΩ ── Ring / DATA
GPIO22 ── BUSY 5 V transistor TX ── 2.2 kΩ ── Tip  / BUSY
GPIO27 ◀────── 10 kΩ / 10 kΩ divider ─────── Ring / DATA
GPIO23 ◀────── 10 kΩ / 10 kΩ divider ─────── Tip  / BUSY
GND    ────────────────────────────────────── Sleeve / GND
```

The DATA and BUSY TX circuits are **separate**. An earlier experimental DATA TX circuit showed cross-coupling and is not part of the validated configuration. This text records the functional wiring, not a complete transistor-stage construction drawing; do not rebuild the stages solely from this simplified diagram.

### Raspberry Pi validation results

| Test | Observed result |
|---|---|
| DATA TX/RX loopback | 1,000 transitions, zero mismatches |
| BUSY TX/RX loopback | 1,000 transitions, zero mismatches |
| Dual-channel loopback | 1,000 transitions, zero mismatches |
| Waveform test | 20 transitions, zero mismatches; maximum completion lateness 251.6 µs |
| Live KR-V6070 input selection | Five individually transmitted commands produced the expected receiver input changes |

| XS8 command | Observed receiver input | Result |
|---|---|---|
| `0x82` | TUNER | Verified |
| `0x83` | PHONO | Verified |
| `0x84` | CD | Verified |
| `0x85` | TAPE 1 | Verified |
| `0x86` | TAPE 1 | Verified |

The distinction, if any, between `0x85` and `0x86` is **not established** by these observations. Successful return from `xs8_send` alone does not establish receiver acceptance; the entries above were confirmed by observing the receiver switch inputs.

The transmitter was added in commit `a2111e7` (`feat(xs8): add Raspberry Pi XS8 transmitter`). These results validate **individual input-selection commands**, not long-term operation, simultaneous bus participants, CD transport sequences, volume/mute sequences, or end-to-end CEC integration on the Pi-only path.

## Observed electrical behavior and timing

- **Bus idle:** DATA and BUSY were observed LOW in the working setup. BUSY goes HIGH around a frame; measured captures show DATA rising about 1.15 ms later. [Captured edges](../research/captures/xs-session2.csv), [research summary](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)
- **Arduino line behavior:** The Arduino asserts HIGH with `OUTPUT HIGH` and represents LOW by switching its TX pin to `INPUT` (high impedance). It releases both lines after each frame. The Pi's external transistor drivers implement corresponding assert-HIGH/release behavior. Do not substitute an assumed open-drain, active-LOW, or pull-up circuit without separate measurements. [Arduino firmware](../xs8-bridge/xs8-bridge.ino), [Pi transmitter](../xs8_send.c)
- **Nominal frame timing:** BUSY lead about **1,150 µs**; DATA start HIGH about **9,800 µs**; eight bits with DATA released for about **4,900 µs** or **9,800 µs**, each followed by DATA HIGH for about **4,900 µs**. DATA is released for **750 µs** before BUSY is released. This ending sequence is a working experiment, not a proven universal Kenwood requirement. [Firmware constants](../xs8-bridge/xs8-bridge.ino), [timing notes](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)
- **Bus-free check:** The Arduino waits for BUSY LOW for **500 µs**, with a **100 ms** timeout, before transmitting. Its `500` ADC threshold is a software classification value, not a specified voltage threshold for all hardware. A0 is configured as an input but is not used by the bridge to decode responses. The Pi transmitter also checks BUSY before sending. [Arduino firmware](../xs8-bridge/xs8-bridge.ino), [Pi transmitter](../xs8_send.c)
- **Signal levels:** Research notes say HIGH ADC readings differed among receiver, changer, and Arduino; they do not establish calibrated rail limits for each device. An archived bench capture repeatedly printed `LOW: 0  0.000 V` and `HIGH: 974  3.142 V`. These voltages were *calculated* by `xs-amplitude2.ino` using an assumed **3.3 V ADC reference** in a separate TX bench arrangement with a 47 kΩ pull-down. They are **not** a measured KR-V6070 BUSY or DATA maximum or an input-voltage rating. [Bench readings](../research/captures/xs-amplitude.csv), [measurement sketch](../research/sketches/xs-amplitude2.ino), [level caveat](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)

The earlier `xs-amplitude.ino` also describes a 10 kΩ/10 kΩ divider and an assumed 3.3 V ADC reference. The archive does not tie its calculated bus voltage to a calibrated live capture or establish acceptable voltage/current ranges for other Kenwood models. [Divider sketch](../research/sketches/xs-amplitude.ino)

## Verify wiring before transmitting

### Arduino Micro configuration

1. **With everything unpowered and disconnected**, identify sleeve, tip, and ring on the actual plug with a continuity meter. Check that sleeve reaches only the intended common ground, tip reaches BUSY, and ring reaches DATA. Do not rely on wire color.
2. Trace the Arduino wiring end-to-end: **D2 → 2.2 kΩ → ring/DATA**, **D3 → 2.2 kΩ → tip/BUSY**, A0 to DATA, A5 to BUSY, and GND to sleeve. Confirm no short between DATA, BUSY, ground, or an unintended supply rail. In this configuration, the Pi connects by USB only.
3. With Arduino TX pins released/high impedance, power the devices for a **passive** check. Observe known receiver or CD activity. The archived baseline is idle LOW plus a BUSY envelope and DATA transitions. If mapping or levels differ, stop before enabling TX. [Passive sniffer](../research/sketches/xs-raw-adc.ino), [edge capture](../research/captures/xs-session2.csv)
4. Before an active frame, confirm that the installed firmware is the [high-impedance-LOW bridge](../xs8-bridge/xs8-bridge.ino), that D2/D3 have their series resistors, and that BUSY is not asserted. A serial write or Arduino `OK` is not proof the receiver accepted a frame. [Software data path](architecture.md)

The archived [pin-mapper sketch](../research/sketches/xs-pin-mapper.ino) deliberately drives a line HIGH for two seconds; it is an **active** bench experiment, not a passive continuity test. Likewise, `xs-lo2.ino` was a **no-Kenwood-connected** bench test with reversed A0/A5 labels and does not supersede the live wiring above. [Bench test header](../research/sketches/xs-lo2.ino)

### Raspberry Pi GPIO configuration

1. With power disconnected, verify the TRS contact mapping and common ground. Trace GPIO4 to the **DATA** TX stage and ring, GPIO22 to the **BUSY** TX stage and tip, GPIO27 to the **DATA** divider, and GPIO23 to the **BUSY** divider.
2. Confirm that each TX stage has its own **2.2 kΩ output series resistor**, that the Pi GPIO pins are isolated from the 5 V bus by the external circuitry, and that both RX paths use voltage dividers. Check for unintended shorts or cross-coupling between the two TX stages.
3. Before connecting to the receiver, verify that both TX control GPIOs are LOW/released at idle. Do not drive a Pi GPIO directly onto an XS8 line.
4. Start with a documented, state-observable command and confirm the **receiver's actual response**, not merely a successful process exit. The known single-command input-selection tests are listed above.

The Pi procedure documents the **validated wiring arrangement**; it is not a substitute for inspecting the actual transistor stages or for a complete electrical safety analysis.

## Safety limits of the tested circuits

- The Arduino and Raspberry Pi interfaces were experimentally validated on the recorded **KR-V6070** setup. Neither provides an electrical specification proving compatibility with other Kenwood receivers, accessories, supply voltages, or cable arrangements.
- TX must start released and must not force LOW against another bus participant. BUSY checking reduces the risk of starting while the observed bus is occupied, but is not a hardware interlock and cannot eliminate every collision or wiring fault. [Arduino transmitter](../xs8-bridge/xs8-bridge.ino), [Pi transmitter](../xs8_send.c)
- Continuous volume commands can keep changing volume after a single frame. Any transmitter must send XS8 **volume STOP `0x09` before context exit `0x07`**; `xs8-service` and the Arduino bridge both do. If BUSY stays occupied, no watchdog can guarantee STOP delivery. CEC volume and MUTE have since been validated on the Pi GPIO path through `xs8-service` ([project status](project-status.md)). Still verify at a controlled level after any wiring change. [Control sequences](xs8-protocol.md), [watchdog limitation](../xs8-bridge/xs8-bridge.ino)
- Avoid exploratory bytes during hardware bring-up. Research found a command family that persistently locks front-panel controls; absence of an RX frame does not establish a harmless NOP. Use documented commands with known context and state. [Command registry](../research/COMMAND_REGISTRY.md)
- The published dimensions, voltages, and timing are **observations or software settings**, not component ratings or a completed signal-integrity analysis. Raw captures establish transitions and approximate intervals, while many receiver effects survive only as written bench notes. [Research history](../research/RESEARCH_HISTORY.md)
