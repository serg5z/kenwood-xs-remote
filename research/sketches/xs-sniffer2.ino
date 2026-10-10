constexpr uint8_t ADC_PIN = A0;
constexpr uint32_t CAPTURE_MS = 5000;

constexpr uint16_t HIGH_THRESHOLD = 600;
constexpr uint16_t LOW_THRESHOLD  = 200;

struct Event {
  uint32_t us;
  uint16_t adc;
  bool high;
};

constexpr uint16_t MAX_EVENTS = 100;
Event events[MAX_EVENTS];

void setup() {
  Serial.begin(115200);
  pinMode(ADC_PIN, INPUT);

  while (!Serial) {}
  Serial.println("# READY");
  Serial.println("# send 'a' to arm");
}

void capture() {
  Serial.println("# ARMED -- operate INPUT now");

  uint16_t count = 0;
  uint16_t maxValue = 0;

  // Establish initial state.
  uint16_t v = analogRead(ADC_PIN);
  bool high = (v > HIGH_THRESHOLD);

  uint32_t startUs = micros();
  uint32_t startMs = millis();

  while (millis() - startMs < CAPTURE_MS) {
    v = analogRead(ADC_PIN);

    if (v > maxValue)
      maxValue = v;

    bool changed = false;

    if (!high && v > HIGH_THRESHOLD) {
      high = true;
      changed = true;
    }
    else if (high && v < LOW_THRESHOLD) {
      high = false;
      changed = true;
    }

    if (changed && count < MAX_EVENTS) {
      events[count].us = micros() - startUs;
      events[count].adc = v;
      events[count].high = high;
      count++;
    }
  }

  Serial.println("# DONE");
  Serial.print("# max ");
  Serial.println(maxValue);

  Serial.print("# events ");
  Serial.println(count);

  Serial.println("# time_us state adc");

  for (uint16_t i = 0; i < count; i++) {
    Serial.print(events[i].us);
    Serial.print(' ');
    Serial.print(events[i].high ? "HIGH" : "LOW");
    Serial.print(' ');
    Serial.println(events[i].adc);
  }

  Serial.println("# END");
  Serial.println("# send 'a' to arm");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();

    if (c == 'a' || c == 'A')
      capture();
  }
}
