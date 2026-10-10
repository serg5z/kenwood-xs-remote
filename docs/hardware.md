# Kenwood XS Remote hardware

This document describes the **tested KR-V6070 and Arduino Micro wiring**, not a certified interface for every SYSTEM CONTROL device. The [XS8 protocol reference](xs8-protocol.md) covers frame encoding and commands; the [architecture](architecture.md) covers the CEC-to-serial software path.

## Components and connections

| Component | Role in this project |
|---|---|
| Raspberry Pi B+ | Runs the `libCEC` host program and sends one-character commands over USB serial. The current host opens `/dev/ttyACM0`; it does not connect a Pi GPIO directly to XS8. [Host code](../main.cpp) |
| Arduino Micro, 5 V setup | Converts serial commands into timed XS8 frames. Its USB connection to the Pi also provides the serial link. [Working firmware](../xs8-bridge/xs8-bridge.ino), [bench summary](../research/notes/kenwood-xs-reverse-engineering-reviewed.md) |
| Kenwood KR-V6070 | Receiver whose SYSTEM CONTROL/XS connector and responses were investigated. Some research tests also used a connected Kenwood CD player/changer. [Passive observations](../research/notes/kenwood-xs8-sniffing-notes.md) |

The tested connector is a **TRS** plug:

| Plug contact | XS signal | Arduino-side connection |
|---|---|---|
| Sleeve | GND | Common ground |
| Tip | `S.BUSY` / BUSY | D3 through **2.2 kΩ** to the BUSY node; A5 senses that same node |
| Ring | `S.DATA` / DATA | D2 through **2.2 kΩ** to the DATA node; A0 senses that same node |

```text
Raspberry Pi B+ ── USB ── Arduino Micro
                         D2 ── 2.2 kΩ ──┬── TRS ring: S.DATA
                         A0 ────────────┘
                         D3 ── 2.2 kΩ ──┬── TRS tip: S.BUSY
                         A5 ────────────┘
                         GND ────────────── TRS sleeve: GND
                                            Kenwood KR-V6070
```

The 2.2 kΩ resistors are **in the TX paths**. A0 and A5 sense the XS-side nodes, as shown in the firmware wiring and research notes; they are not substitutes for those resistors. No Pi GPIO-to-XS wiring is part of the tested system. [Arduino pin declarations](../xs8-bridge/xs8-bridge.ino), [physical mapping](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)

## Observed electrical behavior and timing

- **Bus idle:** DATA and BUSY were observed LOW in the working setup. BUSY goes HIGH around a frame; the measured captures show DATA rising about 1.15 ms later. [Captured edges](../research/captures/xs-session2.csv), [research summary](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)
- **How this transmitter drives the bus:** Arduino asserts HIGH with `OUTPUT HIGH` and represents LOW by changing its TX pin to `INPUT` (high impedance). It releases both lines after each frame. This is the method used by the working firmware; do not substitute an assumed open-drain, active-LOW, or pull-up circuit without separate measurements. [Firmware transmit routine](../xs8-bridge/xs8-bridge.ino)
- **Nominal frame timing:** BUSY lead about **1,150 µs**; DATA start HIGH about **9,800 µs**; eight bits with a DATA LOW interval of about **4,900 µs** or **9,800 µs**, each followed by DATA HIGH about **4,900 µs**. The firmware releases DATA, waits **750 µs**, then releases BUSY. Its ending sequence is a working experiment, not a proven universal Kenwood requirement. [Firmware constants](../xs8-bridge/xs8-bridge.ino), [timing notes](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)
- **Bus-free check:** The firmware waits for sensed BUSY LOW for **500 µs**, with a **100 ms** timeout, before transmitting. Its `500` ADC threshold is a software classification value, not a specified voltage threshold for all hardware. A0 is configured as an input in the bridge but is not used there to decode responses. [Firmware](../xs8-bridge/xs8-bridge.ino)
- **Signal levels:** Research notes say HIGH ADC readings differed among the receiver, changer, and Arduino; they do not provide calibrated rail limits for each device. An archived bench capture repeatedly printed `LOW: 0  0.000 V` and `HIGH: 974  3.142 V`. Those volts were *calculated* by `xs-amplitude2.ino` from an assumed **3.3 V ADC reference** in a separate TX bench arrangement with a 47 kΩ pull-down. They are **not** a measured KR-V6070 BUSY or DATA maximum and should not be used as an input-voltage rating. [Bench readings](../research/captures/xs-amplitude.csv), [measurement sketch](../research/sketches/xs-amplitude2.ino), [level caveat](../research/notes/kenwood-xs-reverse-engineering-reviewed.md)

The earlier `xs-amplitude.ino` also describes a 10 kΩ/10 kΩ divider and an assumed 3.3 V ADC reference. The archive does not tie its calculated bus voltage to a calibrated live capture or establish acceptable voltage/current ranges for other Kenwood models. [Divider sketch](../research/sketches/xs-amplitude.ino)

## Verify the wiring before transmitting

1. **With everything unpowered and disconnected**, identify sleeve, tip, and ring on the actual plug with a continuity meter. Check that sleeve reaches only the intended common ground, tip reaches the BUSY lead, and ring reaches the DATA lead. Do not rely on wire color or the appearance of a cable assembly.
2. Trace each Arduino connection end-to-end: **D2 → 2.2 kΩ → ring/DATA**, **D3 → 2.2 kΩ → tip/BUSY**, A0 to the DATA node, A5 to the BUSY node, and Arduino GND to sleeve. Check the resistor values and confirm no short between DATA, BUSY, ground, or an unintended supply rail. Confirm that the Pi connects by USB only.
3. With the Arduino TX pins still released/high impedance, power the devices for a **passive** check. Measure the line levels relative to sleeve and observe a known receiver or CD action. The archived baseline is idle LOW plus a BUSY envelope and DATA transitions; if the observed mapping or levels differ, stop and re-identify the connection before enabling TX. [Passive sniffer](../research/sketches/xs-raw-adc.ino), [edge capture](../research/captures/xs-session2.csv)
4. Before the first active frame, confirm that the installed firmware is the [current high-impedance-LOW bridge](../xs8-bridge/xs8-bridge.ino), that D2/D3 have their series resistors, and that BUSY is not already asserted. Transmit only with a known receiver state and a way to observe the receiver response. A successful serial write or an Arduino `OK` line is not proof that the receiver accepted the XS8 frame. [Software data path](architecture.md)

The archived [pin-mapper sketch](../research/sketches/xs-pin-mapper.ino) can help check which Arduino TX pin affects which sensed node, but it deliberately drives a line HIGH for two seconds. Treat it as an active bench experiment, not as a passive continuity test or a first step on an unidentified connector. Likewise, `xs-lo2.ino` was a **no-Kenwood-connected** bench test with reversed A0/A5 labels; it does not supersede the live wiring above. [Bench test header](../research/sketches/xs-lo2.ino)

## Safety limits of the tested circuit

- The wiring and timing were validated experimentally on the recorded **KR-V6070/Arduino Micro setup**. The archive contains no electrical specification proving compatibility with other Kenwood receivers, accessories, supply voltages, or cable arrangements. Check the actual connector and electrical levels rather than assuming all SYSTEM CONTROL ports match.
- The TX pins must start released and must not be configured to force LOW against another participant. The firmware uses BUSY to avoid starting while the observed bus is occupied; that check is not a hardware interlock and does not eliminate every collision or wiring fault. [Transmit code](../xs8-bridge/xs8-bridge.ino)
- Continuous volume commands can keep changing volume after a single frame. The bridge must send XS8 **volume STOP `x09` before context exit `x07`**. If BUSY stays occupied, its watchdog cannot guarantee that STOP is transmitted. Verify volume behavior at a controlled level before routine use. [Control sequences](xs8-protocol.md), [watchdog limitation](../xs8-bridge/xs8-bridge.ino)
- Avoid exploratory bytes during hardware bring-up. The research found a family that persistently locks front-panel controls; an absent RX frame does not establish a harmless NOP. Use only commands with documented context and state. [Command registry](../research/COMMAND_REGISTRY.md)
- The published dimensions, voltages, and timing above describe **observations or firmware settings**, not component ratings or a completed signal-integrity analysis. The raw captures establish transitions and approximate intervals, while many receiver effects survive only as written bench notes. [Research history](../research/RESEARCH_HISTORY.md)
