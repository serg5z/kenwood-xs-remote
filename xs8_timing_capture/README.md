# xs8_timing_capture — Arduino Micro edge recorder

[Sketch](xs8_timing_capture.ino) for the **5 V, 16 MHz Arduino Micro / ATmega32U4**.
A separate sketch: `sample5/sample5.ino` and all Raspberry Pi software remain
unchanged. It captures two external CHANGE interrupts, then exports raw CSV over
**native USB Serial**, never the UART (`Serial1`) occupying D0/D1. It performs
no XS8 transmission, decoding, recovery or receiver control.

## Wiring and voltage limits

### Passive H12 recording — normal build

```text
H12 sense nodes                          Arduino Micro
BUSY_RX (same node as Pi GPIO26) -------- D0 / PD2 / INT2   INPUT, pull-up OFF
DATA_RX (same node as Pi GPIO13) -------- D1 / PD3 / INT3   INPUT, pull-up OFF
GND ------------------------------------ GND
                                         USB ---------- Mac (power + CSV)
                                         D4             UNCONNECTED / INPUT
```

D0 is **BUSY**, D1 is **DATA**; this is different from sample5's A0/A1 wiring.
These are taps of the already divided RX nodes, not Pi TX outputs. No Arduino
5 V or 3.3 V supply pin connects to the Pi, H12 supply, or XS8 signal lines.
Never leave a self-test output jumper installed when connecting H12.

Use the **normal passive build** for H12. D0/D1 are always inputs without pull-ups,
even in self-test. The normal build also leaves D4 as an input and compiles out
all stimulus generation. The sketch does not electrically verify its wiring or
supply state. Connecting a recorder adds input leakage/capacitance; verify that
it does not materially alter the RX levels/waveform under investigation.

The Micro operates at 5 V and has 2560 bytes of SRAM. Its D0/D1 map to PD2/INT2
and PD3/INT3, as defined by the [Micro variant](https://github.com/arduino/ArduinoCore-avr/blob/1.8.8/variants/micro/pins_arduino.h)
and [shared Leonardo mapping](https://github.com/arduino/ArduinoCore-avr/blob/1.8.8/variants/leonardo/pins_arduino.h).
See also [Arduino Micro specifications](https://docs.arduino.cc/hardware/micro/).

For these ordinary digital inputs, the ATmega32U4 datasheet **Table 29-1** gives:

| Guaranteed logic condition | Formula | At measured VCC = 5.00 V |
|---|---|---:|
| LOW maximum | `0.2 × VCC − 0.1 V` | 0.90 V |
| HIGH minimum | `0.2 × VCC + 0.9 V` | 1.90 V |

The intervening band is unspecified. These are **LVTTL** limits; do not substitute
the different XTAL/RESET or TWI input limits from other tables. The previously
reported H12 HIGH of 2.81 V is compatible in principle. The earlier 2.01 V HIGH
has much less margin. These statements do not verify either channel's present
voltage or noise margin. Measure actual Micro VCC and both RX LOW/HIGH levels.
[ATmega16U4/32U4 datasheet, §29.1–29.2, printed p.383–384](https://ww1.microchip.com/downloads/en/devicedoc/atmel-7766-8-bit-avr-atmega16u4-32u4_datasheet.pdf#page=383)

Operate inputs within **GND to actual VCC**, with adequate noise margin. The
absolute stress limits, −0.5 V to VCC+0.5 V, are **not normal operating targets**.
Do not apply negative pulses, higher-voltage bus nodes, or powered inputs to an
unpowered Micro. Input protection can back-power an unpowered board. Connect or
rewire only with equipment unpowered, establish common GND, and power the Micro
before exciting the disconnected fixture. This is experimental instrumentation,
not an isolated or universally compatible Kenwood interface. No H12 board change
or receiver-connected test is authorized by these instructions.

## Capture and CSV

1. Upload the passive build later, with H12 disconnected during programming.
2. Open native USB Serial at 115200 and select a newline line ending. USB CDC
   does not use this setting as a D0/D1 UART baud clock.
3. Send `H` for help. Nothing captures or transmits automatically on boot.
4. Send `S` once. An ARMING message is flushed **before** arming. Then the sketch
   stays silent for a **5-second window**. Start the separately authorized short
   experiment promptly after that message. No experiment is launched by Arduino.
5. After the window, interrupts are detached and the buffer/metadata export.
   Save all output. `D` dumps that same completed buffer again; a new `S` replaces
   it. `H` and `D` never arm capture. Commands typed during capture wait until it
   ends; do not queue repeated commands.

The main loop ends the window cooperatively; actual elapsed duration is reported.
This is a time window, not a decoded frame-boundary trigger. Allow enough time
for the whole exchange; absence of activity does not end the recording early.
The buffer retains the **first 192 ISR observations**, without wrapping. It can
hold nine nominal 20-edge XS8 frames plus 12 further events. Other traffic or
noise consumes the same space. On full buffer, capture continues to the deadline
and a saturating 32-bit overflow counter counts further ISR observations that
could not be stored. No live streaming or dynamic growth is attempted.

```csv
time_us,channel,edge,same_level,overflow_total
```

- `time_us`: unsigned microseconds relative to this capture's Arduino start.
- `channel`: `BUSY` (D0) or `DATA` (D1).
- `edge`: rising/falling **inferred from the pin level sampled in the ISR**.
- `same_level`: 1 if that sample equals the preceding sample on that channel,
  including the initial level. Such an entry has unresolved edge polarity;
  retain it rather than treating it as a verified rising/falling transition.
- `overflow_total`: **final** buffer-drop count repeated in each row, not a
  historical count at the row's timestamp. It is zero only for no RAM overflow.

Lines beginning `#` contain start `micros()`, elapsed duration, initial levels,
counts, capacity, buffer size and pending external-interrupt flags at stop.
`pending_irq_flags_at_stop` is a raw flag mask (INT2=4, INT3=8), not a lost-edge
count. A nonzero flag can indicate activity at the capture cutoff. Buffer overflow,
same-level samples, pending flags, or incomplete exchange coverage must be kept
with the data; none is silently repaired. `# END` terminates one export.

Illustrative self-test excerpt, **not a measured hardware result**:

```text
# initial_busy=0 initial_data=0
# events=32 capacity=192 buffer_bytes=1152 overflow_total=0
# same_level_samples=0 pending_irq_flags_at_stop=0
time_us,channel,edge,same_level,overflow_total
2004,BUSY,rising,0,0
6912,BUSY,falling,0,0
16724,BUSY,rising,0,0
21632,BUSY,falling,0,0
...
# END
# SELFTEST channel=BUSY transitions=32 high_us=4900 low_us=9800 tolerance_us=100 result=PASS
```

The full test has 32 alternating entries; the ellipsis above is not a CSV row.

## macOS build (no board needed)

Install Arduino CLI and the Arduino AVR core if absent. These commands change
only the Mac toolchain/build output, not any board:

```sh
brew install arduino-cli
arduino-cli core update-index
arduino-cli core install arduino:avr@1.8.8
```

From the repository root, build into separate ignored directories:

```sh
# Normal recorder: stimulus code absent.
arduino-cli compile --fqbn arduino:avr:micro --warnings all \
  --build-path .build/arduino-xs8-timing-passive xs8_timing_capture

# Disconnected jumper fixture only; explicitly includes self-test generation.
arduino-cli compile --fqbn arduino:avr:micro --warnings all \
  --build-property compiler.cpp.extra_flags=-DXS8_ENABLE_SELF_TEST=1 \
  --build-path .build/arduino-xs8-timing-selftest xs8_timing_capture
```

Arduino IDE alternative: open `xs8_timing_capture.ino`, select **Arduino Micro**.
The default is passive. To enable the fixture-only test in the IDE, explicitly
set `XS8_ENABLE_SELF_TEST` to 1 in a local test copy; do not mistake that image
for the passive image. CLI builds above keep the original default unchanged.

Later operator-only upload example, **H12 and receiver wiring disconnected**:

```sh
# Substitute the actual Micro port, not a Raspberry Pi or another device.
arduino-cli upload --fqbn arduino:avr:micro --port /dev/cu.usbmodemPORT \
  --input-dir .build/arduino-xs8-timing-selftest xs8_timing_capture
```

Before any H12 connection, remove the D4 jumper and upload the **passive** image
by substituting `.build/arduino-xs8-timing-passive`. `H` must report PASSIVE BUILD.
A successful compile/upload is not a successful physical timing test.

## Autonomous jumper self-test — before H12

No Pi/H12/Kenwood connections are present during this test. The only powered
hardware is the Micro on USB. The explicit command records operator intent;
software cannot check physical disconnection.

### Test BUSY input

With USB disconnected, wire:

```text
Micro D4 ---- [1 kΩ recommended] ---- D0 BUSY
Micro GND ------------------------- D1 DATA (inactive input)
Micro USB ------------------------- Mac (connect after wiring)
```

Upload the self-test image with H12 completely disconnected, then send:

```text
H
T BUSY DISCONNECTED
```

The generator drives D4 LOW, waits 5 ms, arms both input interrupts, waits about
2 ms, then produces **32 alternating transitions**, HIGH for nominal **4900 µs**,
LOW for nominal **9800 µs**. It ends LOW, stops capture and returns D4 to INPUT
before printing CSV and the self-test result. It never repeats automatically.
D0/D1 remain inputs throughout. Main-loop delays do not disable interrupts;
no delay or Serial operation runs in a capture handler.

### Test DATA input

Disconnect USB, move the fixture jumper and ground connection:

```text
Micro D4 ---- [1 kΩ recommended] ---- D1 DATA
Micro GND ------------------------- D0 BUSY (inactive input)
```

Reconnect USB and send `T DATA DISCONNECTED`. Never connect D4 to Pi GPIO or H12:
its output is approximately Micro VCC, not a 3.3 V-safe source. The optional
series resistor limits accidental fixture shorts; it is not a level shifter.

### Acceptance

Require PASS on **both** channels, and preserve both full CSV exports:

- Exactly 32 alternating edges on the selected channel, none on the inactive one.
- No buffer overflow, same-level samples or pending interrupt flags at stop.
- All 31 adjacent-edge intervals within ±100 µs of their intended duration:
  16 HIGH intervals near 4900 µs and 15 LOW intervals near 9800 µs.
- A missing/wrong jumper, extra activity, bad interval or wrong channel yields
  FAIL, with the raw capture retained. Investigate before connecting H12; do not
  weaken acceptance thresholds to manufacture PASS.

After testing, power off, remove **all fixture jumpers**, flash the passive image,
verify PASSIVE BUILD using `H`, and only then arrange separately approved passive
H12 observation with measured voltage compatibility. No hardware self-test was
executed during development.

## Memory, timestamp resolution and latency

Build results using Arduino AVR core 1.8.8 / AVR GCC 7.3.0:

| Image | Flash / 28672 bytes | Static SRAM / 2560 bytes | SRAM left for stack |
|---|---:|---:|---:|
| Passive | 6586 | 1378 | 1182 |
| Self-test | 7712 | 1416 | 1144 |

The 192 × 6-byte edge buffer is **1152 bytes**. The other globals include a fixed
32-byte command buffer, counters and core USB/timer state. Remaining SRAM is not
an independently measured worst-case stack margin. There is no sketch heap use,
`String`, per-event allocation, ring overwrite, decoding or ISR formatting.
Both images compile with `--warnings all`; core 1.8.8 reports existing unused
`tag` parameter warnings in `new.cpp`. An all-code `-Werror` attempt consequently
fails in that upstream file; the sketch's initial signed-comparison warning was
fixed. No installed core was modified or warning policy hidden.

At 16 MHz, the [AVR core's `micros()`](https://github.com/arduino/ArduinoCore-avr/blob/1.8.8/cores/arduino/wiring.c)
uses Timer0 at 16 MHz /64: nominal **4 µs resolution**, not 1 µs. ISR entry,
Arduino's callback wrapper and `micros()` sampling add latency. Two close channel
changes are serviced sequentially, not captured simultaneously; interrupt priority
can determine row order. Timestamping occurs just before the pin-level read, not
at a hardware-latched electrical edge. Actual latency/jitter was **not measured**.

Timer0 and USB interrupts remain active. Other ISRs delay edge service, and a
single pending external-interrupt flag cannot count arbitrarily many changes.
Coalesced pulses may leave no overflow or same-level indication. The counter
therefore measures RAM overflow only, not all missing physical edges. If interrupts
are blocked across multiple Timer0 overflows (nominal 1.024 ms each), `micros()`
can also lose time. Handlers here are kept short; this is not a lossless logic
analyzer or hard real-time guarantee. Oscillator error affects absolute timing;
self-test generation and capture share that oscillator and cannot calibrate it.

Unsigned subtraction handles a `micros()` rollover within a short capture. Arduino
and Pi clocks are independent: align captures using the observed BUSY/DATA pattern,
not matching numeric timestamps. Preserve original ISR service order, ties and
ambiguous samples without sorting them into a presumed electrical order. Use the
unchanged ADC recorder or independent analog instrumentation when voltage shape,
threshold crossings or very short disturbances are the question.
