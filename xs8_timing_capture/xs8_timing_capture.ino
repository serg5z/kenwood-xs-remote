/* Arduino Micro only: passive D0/BUSY + D1/DATA interrupt recorder.
 * USB Serial, never Serial1. See README.md before connecting to H12.
 * Self-test output is absent from the default build. */
#include <Arduino.h>
#include <avr/io.h>
#include <util/atomic.h>
#include <string.h>

#if !defined(ARDUINO_AVR_MICRO) || !defined(__AVR_ATmega32U4__) || F_CPU != 16000000UL
#error "Select Arduino Micro, ATmega32U4 at 16 MHz"
#endif
#ifndef XS8_ENABLE_SELF_TEST
#define XS8_ENABLE_SELF_TEST 0
#endif
#if XS8_ENABLE_SELF_TEST != 0 && XS8_ENABLE_SELF_TEST != 1
#error "XS8_ENABLE_SELF_TEST must be 0 or 1"
#endif

constexpr uint8_t BUSY_PIN = 0;  // PD2 / INT2
constexpr uint8_t DATA_PIN = 1;  // PD3 / INT3
constexpr uint8_t TEST_OUT = 4;  // fixture jumper only, never connected to H12
constexpr uint8_t BUSY_CHANNEL = 0;
constexpr uint8_t DATA_CHANNEL = 1;
constexpr uint8_t LEVEL_HIGH = 1;
constexpr uint8_t SAME_LEVEL = 2;
constexpr uint8_t INPUT_IRQ_FLAGS = _BV(INTF2) | _BV(INTF3);
constexpr uint16_t MAX_EVENTS = 192;
constexpr uint32_t CAPTURE_US = 5000000UL;
constexpr uint8_t TEST_TRANSITIONS = 32;
constexpr uint16_t TEST_TOLERANCE_US = 100;

struct Edge {
  uint32_t time_us;
  uint8_t channel;
  uint8_t flags;
};
static_assert(sizeof(Edge) == 6, "Unexpected AVR event layout");
volatile Edge edges[MAX_EVENTS]; // 1152 bytes; no heap or buffer wrap
volatile uint16_t eventCount = 0;
volatile uint32_t overflowCount = 0;
volatile uint32_t sameLevelCount = 0;
volatile uint32_t captureStartUs = 0;
volatile bool capturing = false;
volatile uint8_t previousLevel[2] = {0, 0};
uint32_t captureEndUs = 0;
uint8_t initialBusy = 0, initialData = 0, pendingAtStop = 0;
char command[32];
uint8_t commandLength = 0;
bool commandTooLong = false;
bool haveCapture = false;

// Called only by non-nested external interrupt callbacks. Sampling level is not
// hardware-latched polarity: very short/coalesced changes can be ambiguous.
void recordChange(uint8_t channel, uint8_t portMask) {
  if (!capturing) return;
  const uint32_t timestamp = micros();
  const uint8_t high = (PIND & portMask) ? 1 : 0;
  const bool same = high == previousLevel[channel];
  previousLevel[channel] = high;
  if (same && sameLevelCount != UINT32_MAX) ++sameLevelCount;
  const uint16_t index = eventCount;
  if (index >= MAX_EVENTS) {
    if (overflowCount != UINT32_MAX) ++overflowCount;
    return;
  }
  edges[index].time_us = timestamp - captureStartUs; // unsigned wrap-safe
  edges[index].channel = channel;
  edges[index].flags = (high ? LEVEL_HIGH : 0) | (same ? SAME_LEVEL : 0);
  eventCount = index + 1;
}

void busyChanged() { recordChange(BUSY_CHANNEL, _BV(PD2)); }
void dataChanged() { recordChange(DATA_CHANNEL, _BV(PD3)); }

void startCapture() {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    eventCount = 0;
    overflowCount = 0;
    sameLevelCount = 0;
    pendingAtStop = 0;
    const uint8_t pins = PIND;
    initialBusy = previousLevel[BUSY_CHANNEL] = (pins & _BV(PD2)) ? 1 : 0;
    initialData = previousLevel[DATA_CHANNEL] = (pins & _BV(PD3)) ? 1 : 0;
    captureStartUs = micros();
    // Discard pre-arm pending flags, then enable both CHANGE interrupts.
    EIFR = INPUT_IRQ_FLAGS;
    attachInterrupt(digitalPinToInterrupt(BUSY_PIN), busyChanged, CHANGE);
    attachInterrupt(digitalPinToInterrupt(DATA_PIN), dataChanged, CHANGE);
    capturing = true;
  }
}

void stopCapture() {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    captureEndUs = micros();
    capturing = false;
    pendingAtStop = EIFR & INPUT_IRQ_FLAGS;
    detachInterrupt(digitalPinToInterrupt(BUSY_PIN));
    detachInterrupt(digitalPinToInterrupt(DATA_PIN));
    haveCapture = true;
  }
}

void exportCapture() {
  // Interrupts detached: the shared buffer/counters are now stable.
  Serial.print(F("# start_micros=")); Serial.println(captureStartUs);
  Serial.print(F("# duration_us=")); Serial.println(captureEndUs - captureStartUs);
  Serial.print(F("# initial_busy=")); Serial.print(initialBusy);
  Serial.print(F(" initial_data=")); Serial.println(initialData);
  Serial.print(F("# events=")); Serial.print(eventCount);
  Serial.print(F(" capacity=")); Serial.print(MAX_EVENTS);
  Serial.print(F(" buffer_bytes=")); Serial.print(sizeof(edges));
  Serial.print(F(" overflow_total=")); Serial.println(overflowCount);
  Serial.print(F("# same_level_samples=")); Serial.print(sameLevelCount);
  Serial.print(F(" pending_irq_flags_at_stop=")); Serial.println(pendingAtStop);
  Serial.println(F("time_us,channel,edge,same_level,overflow_total"));
  for (uint16_t i = 0; i < eventCount; ++i) {
    Serial.print(edges[i].time_us); Serial.print(',');
    if (edges[i].channel == BUSY_CHANNEL) Serial.print(F("BUSY"));
    else Serial.print(F("DATA"));
    Serial.print(',');
    if (edges[i].flags & LEVEL_HIGH) Serial.print(F("rising"));
    else Serial.print(F("falling"));
    Serial.print(','); Serial.print((edges[i].flags & SAME_LEVEL) ? 1 : 0);
    Serial.print(','); Serial.println(overflowCount);
  }
  Serial.println(F("# END"));
}

#if XS8_ENABLE_SELF_TEST
bool verifySelfTest(uint8_t channel) {
  if (overflowCount || sameLevelCount || pendingAtStop || eventCount != TEST_TRANSITIONS)
    return false;
  for (uint16_t i = 0; i < eventCount; ++i) {
    const bool high = (edges[i].flags & LEVEL_HIGH) != 0;
    if (edges[i].channel != channel || high != (i % 2 == 0)) return false;
    if (i) {
      const uint32_t interval = edges[i].time_us - edges[i - 1].time_us;
      const uint32_t expected = i % 2 ? 4900UL : 9800UL;
      if (interval < expected - TEST_TOLERANCE_US ||
          interval > expected + TEST_TOLERANCE_US) return false;
    }
  }
  return true;
}

void selfTest(uint8_t channel) {
  // Only this explicitly enabled, attended test ever makes D4 an output.
  // Operator must physically disconnect BOTH H12 inputs before this command.
  Serial.println(F("SELFTEST: H12 must be disconnected; D4 jumper fixture only."));
  Serial.flush();
  digitalWrite(TEST_OUT, LOW);
  pinMode(TEST_OUT, OUTPUT);
  delay(5); // settle jumper before arming, not an interrupt-handler operation
  startCapture();
  delayMicroseconds(2000);
  for (uint8_t i = 0; i < TEST_TRANSITIONS; ++i) {
    digitalWrite(TEST_OUT, i % 2 == 0 ? HIGH : LOW);
    delayMicroseconds(i % 2 == 0 ? 4900 : 9800);
  }
  // Last generated edge is LOW. No repeat/recovery on failure.
  stopCapture();
  digitalWrite(TEST_OUT, LOW);
  pinMode(TEST_OUT, INPUT); // release fixture output before reporting
  const bool pass = verifySelfTest(channel);
  exportCapture();
  Serial.print(F("# SELFTEST channel="));
  if (channel == BUSY_CHANNEL) Serial.print(F("BUSY"));
  else Serial.print(F("DATA"));
  Serial.print(F(" transitions=32 high_us=4900 low_us=9800 tolerance_us=100 result="));
  if (pass) Serial.println(F("PASS"));
  else Serial.println(F("FAIL"));
}
#endif

void help() {
  Serial.println(F("xs8_timing_capture: Micro 16MHz; D0=BUSY D1=DATA; USB Serial only."));
  Serial.println(F("S: one passive 5-second capture, then CSV (192 events maximum)."));
#if XS8_ENABLE_SELF_TEST
  Serial.println(F("SELFTEST BUILD: H12 DISCONNECTED; D4 jumper to selected input."));
  Serial.println(F("T BUSY DISCONNECTED or T DATA DISCONNECTED: one bounded self-test."));
#else
  Serial.println(F("PASSIVE BUILD: self-test output disabled; D4 remains INPUT."));
#endif
  Serial.println(F("D: dump last capture again. H: help. Commands need a newline."));
  Serial.println(F("No Serial output while capturing."));
}

void dispatchCommand() {
  if (!strcmp(command, "S")) {
    Serial.println(F("ARMING: capture starts after this message drains; silent for 5 seconds."));
    Serial.flush();
    startCapture();
  } else if (!strcmp(command, "H")) {
    help();
  } else if (!strcmp(command, "D")) {
    if (haveCapture) exportCapture();
    else Serial.println(F("ERR no completed capture."));
#if XS8_ENABLE_SELF_TEST
  } else if (!strcmp(command, "T BUSY DISCONNECTED")) {
    selfTest(BUSY_CHANNEL);
  } else if (!strcmp(command, "T DATA DISCONNECTED")) {
    selfTest(DATA_CHANNEL);
#endif
  } else {
    Serial.println(F("ERR command refused; use H."));
  }
}

void setup() {
  // INPUT clears the pull-up. Never use INPUT_PULLUP or Serial1 on D0/D1.
  pinMode(BUSY_PIN, INPUT);
  pinMode(DATA_PIN, INPUT);
  pinMode(TEST_OUT, INPUT);
  Serial.begin(115200); // native USB CDC, not the UART on D0/D1
}

void loop() {
  if (capturing) {
    if ((uint32_t)(micros() - captureStartUs) >= CAPTURE_US) {
      stopCapture();
      exportCapture();
    }
    return; // no Serial polling/printing or blocking work during passive capture
  }
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (commandTooLong) Serial.println(F("ERR command too long; nothing started."));
      else if (commandLength) {
        command[commandLength] = '\0';
        dispatchCommand();
      }
      commandLength = 0;
      commandTooLong = false;
      if (capturing) break;
    } else if (!commandTooLong) {
      if (static_cast<size_t>(commandLength) + 1U < sizeof(command)) command[commandLength++] = c;
      else commandTooLong = true;
    }
  }
}
