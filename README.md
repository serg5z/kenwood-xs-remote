# Kenwood XS Remote

Kenwood XS Remote lets HDMI-CEC volume controls operate a **Kenwood KR-V6070** receiver through its proprietary **SYSTEM CONTROL (XS8)** interface. A Raspberry Pi runs a `libCEC` host process, and an Arduino Micro converts simple USB serial commands into XS8 bus frames.

The current implementation handles **volume up, volume down, and mute**. The reported working setup uses Panasonic TV and Apple TV remotes through HDMI-CEC. The host code responds to CEC volume and mute key events; it does not identify remote brands or implement input selection or CD transport through CEC.

```text
Panasonic TV / Apple TV remote
             ↓ HDMI-CEC
TV / HDMI-CEC → Raspberry Pi (`libCEC`, main.cpp)
             → USB Serial → Arduino Micro (xs8-bridge.ino)
             → XS8 SYSTEM CONTROL → Kenwood KR-V6070
```

## What is implemented

- The Pi receives CEC volume and mute events and sends one-character commands to the Arduino over `/dev/ttyACM0`.
- The Arduino sends XS8 volume and mute sequences. Volume is continuous until a separate STOP frame; the firmware and host both have 1,200 ms watchdogs to request that stop.
- On volume release, the order is **XS8 volume STOP `x09` followed by context exit `x07`**. Mute uses `x17 → xD8 → x07`. These are current logical TX bytes, not USB serial characters. See the [architecture](docs/architecture.md) and [protocol notes](docs/xs8-protocol.md).

## Requirements

**Hardware:** Raspberry Pi B+, Arduino Micro, Kenwood KR-V6070, a USB connection between Pi and Arduino, an HDMI-CEC path visible to `libCEC`, and a TRS connection to the receiver's SYSTEM CONTROL port. The tested XS8 wiring uses two 2.2 kΩ series resistors: Arduino D2 → DATA (ring), D3 → BUSY (tip), A0 sensing DATA, A5 sensing BUSY, and common ground (sleeve). Check the full [wiring and verification guide](docs/hardware.md) before connecting or transmitting.

**Software:** a C++17 compiler, `libCEC` headers and library for the Pi host, and the Arduino IDE (or another tool capable of uploading the `.ino` sketch to an Arduino Micro). The repository contains no installer, service unit, or build script.

## Build and run manually

1. Wire and verify the XS8 connection using [docs/hardware.md](docs/hardware.md). Keep the Arduino TX pins released while checking the connector and passive bus behavior.
2. In the Arduino IDE, open [xs8-bridge/xs8-bridge.ino](xs8-bridge/xs8-bridge.ino), select **Arduino Micro** and the connected port, then upload the sketch. Connect the Arduino to the Pi by USB. The sketch prints `XS8 BRIDGE 1.0 READY` on its serial port at startup.
3. On the Pi, provide a C++17 compiler and the `libCEC` development headers/library, then compile the host from the repository root:

   ```sh
   g++ -std=c++17 -O2 main.cpp -o cec-xs-bridge -lcec -ldl -pthread
   ```

   This is a manual compiler invocation derived from [main.cpp](main.cpp), not a repository build script or a locally verified package-install command. Library paths and link flags may need to match the Pi's installed `libCEC` build.
4. Confirm that the Arduino is available as **`/dev/ttyACM0`** and that the user can open it. The path is currently fixed in `main.cpp`; this README does not assume another port works without changing the code. Make the CEC adapter available to `libCEC`, then run:

   ```sh
   ./cec-xs-bridge
   ```

   The host sends a serial `S` before opening the CEC adapter. After a fresh Arduino startup, that `S` may produce no XS8 frame because the firmware has no active volume/context state to clear. Press the remote's volume or mute controls to exercise the implemented path; stop the foreground process with Ctrl-C.

The Pi writes commands but does **not** read the Arduino's `OK`/`ERR` text. A successful serial write does not confirm that an XS8 frame reached the receiver. [Architecture details](docs/architecture.md)

## Status and precautions

This is an **experimental hardware interface tested on the documented KR-V6070 setup**, not a general compatibility claim for other Kenwood models. The working transmitter drives HIGH and releases the line for LOW; it must not be replaced with an assumed electrical driver without measuring the target bus. Verify sleeve/tip/ring, resistors, shared ground, idle levels, and the BUSY/DATA mapping before transmitting. [Hardware guide](docs/hardware.md)

A lost volume STOP can leave the receiver changing volume. The watchdogs cannot guarantee delivery while BUSY remains occupied. Other researched XS8 bytes can lock the front panel or change meaning with context, so the production path intentionally exposes only the implemented volume and mute controls. The research archive records observations and unresolved decoder, context, and response questions; it does not contain a complete end-to-end CEC trace. [Protocol limitations](docs/xs8-protocol.md), [research history](research/RESEARCH_HISTORY.md)

## Future direction

A complementary **Web Remote** for receiver input selection and CD transport is planned. It is **not implemented** in the current host or Arduino firmware. Candidate commands and their state-dependent evidence are documented in the [command registry](research/COMMAND_REGISTRY.md).

## Documentation

- [Architecture and serial command flow](docs/architecture.md)
- [Hardware wiring and checks](docs/hardware.md)
- [XS8 engineering specification](docs/xs8-protocol.md)
- [Research archive](research/) — [history](research/RESEARCH_HISTORY.md), [command registry](research/COMMAND_REGISTRY.md), original notes, sketches, and captures
