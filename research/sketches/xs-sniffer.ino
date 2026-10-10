#include <Arduino.h>

constexpr uint8_t DATA_PIN = 2;
constexpr uint8_t BUSY_PIN = 3;

// Сохраняем абсолютное время первого события,
// дальше — интервалы между событиями.
struct Event {
  uint16_t dt;
  uint8_t state;       // bit0 DATA, bit1 BUSY
};

constexpr uint16_t MAX_EVENTS = 400;

volatile Event events[MAX_EVENTS];
volatile uint16_t eventCount = 0;
volatile uint32_t lastTime = 0;
volatile uint32_t firstTime = 0;
volatile bool capturing = false;
volatile bool finished = false;

inline uint8_t readState() {
  uint8_t s = 0;
  if (digitalRead(DATA_PIN)) s |= 1;
  if (digitalRead(BUSY_PIN)) s |= 2;
  return s;
}

void captureISR() {
  if (!capturing)
    return;

  uint32_t now = micros();

  if (eventCount == 0) {
    firstTime = now;
    lastTime = now;
  }

  if (eventCount >= MAX_EVENTS) {
    capturing = false;
    finished = true;
    return;
  }

  uint32_t delta = now - lastTime;

  // 65535 us is plenty for individual edges.
  if (delta > 65535)
    delta = 65535;

  events[eventCount].dt = (uint16_t)delta;
  events[eventCount].state = readState();

  eventCount++;
  lastTime = now;
}

void armCapture() {
  noInterrupts();

  eventCount = 0;
  firstTime = 0;
  lastTime = 0;
  finished = false;
  capturing = true;

  interrupts();

  Serial.println(F("# ARMED"));
}

void dumpCapture() {
  Serial.println(F("# CAPTURE"));
  Serial.print(F("# events "));
  Serial.println(eventCount);
  Serial.println(F("# time_us DATA BUSY"));

  uint32_t t = firstTime;

  for (uint16_t i = 0; i < eventCount; i++) {
    if (i != 0)
      t += events[i].dt;

    Serial.print(t);
    Serial.print(' ');
    Serial.print((events[i].state & 1) ? 1 : 0);
    Serial.print(' ');
    Serial.println((events[i].state & 2) ? 1 : 0);
  }

  Serial.println(F("# END"));
}

void setup() {
  // ONLY for our bench test.
  // These become INPUT before connecting to Kenwood.
  pinMode(DATA_PIN, INPUT_PULLUP);
  pinMode(BUSY_PIN, INPUT_PULLUP);

  Serial.begin(115200);

  attachInterrupt(digitalPinToInterrupt(DATA_PIN), captureISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BUSY_PIN), captureISR, CHANGE);

  delay(500);

  Serial.println(F("# XS capture analyzer"));
  Serial.println(F("# send 'a' to arm"));
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();

    if (c == 'a' || c == 'A')
      armCapture();

    if (c == 'd' || c == 'D') {
      noInterrupts();
      capturing = false;
      interrupts();

      dumpCapture();
    }
  }

  if (finished) {
    noInterrupts();
    finished = false;
    interrupts();

    dumpCapture();
  }
}
