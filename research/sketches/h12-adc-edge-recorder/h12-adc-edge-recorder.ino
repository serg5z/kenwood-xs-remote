#include <Arduino.h>

constexpr uint8_t BUSY_PIN = A0;
constexpr uint8_t DATA_PIN = A1;

// ADC LOW ≈ 0, HIGH ≈ 390.
// Hysteresis avoids repeated triggering near the threshold.
constexpr uint16_t LOW_THRESHOLD = 150;
constexpr uint16_t HIGH_THRESHOLD = 250;

constexpr uint16_t MAX_EVENTS = 160;
constexpr uint32_t CAPTURE_MS = 10000;

struct Edge {
  uint32_t time_us;
  uint8_t channel;
  uint8_t level;
};

Edge events[MAX_EVENTS];
uint16_t eventCount = 0;

struct SampleRange {
  uint16_t minimum;
  uint16_t maximum;
  uint32_t count;
};

struct ADCStats {
  SampleRange all;
  SampleRange high;
  SampleRange low;
  uint32_t ambiguous;
};

ADCStats busyStats;
ADCStats dataStats;

void resetRange(SampleRange &range) {
  range.minimum = 1023;
  range.maximum = 0;
  range.count = 0;
}

void resetStats(ADCStats &stats) {
  resetRange(stats.all);
  resetRange(stats.high);
  resetRange(stats.low);
  stats.ambiguous = 0;
}

void addSample(SampleRange &range, uint16_t sample) {
  if (sample < range.minimum) range.minimum = sample;
  if (sample > range.maximum) range.maximum = sample;
  ++range.count;
}

void collectStats(ADCStats &stats, uint16_t sample) {
  addSample(stats.all, sample);
  // Raw threshold classes, independent of the retained hysteresis state.
  if (sample >= HIGH_THRESHOLD) addSample(stats.high, sample);
  else if (sample <= LOW_THRESHOLD) addSample(stats.low, sample);
  else ++stats.ambiguous; // 151..249 inclusive
}

void printRange(const SampleRange &range) {
  Serial.print(F("count="));
  Serial.print(range.count);
  Serial.print(F(" min="));
  if (range.count) Serial.print(range.minimum);
  else Serial.print(F("NA"));
  Serial.print(F(" max="));
  if (range.count) Serial.println(range.maximum);
  else Serial.println(F("NA"));
}

void printStats(const __FlashStringHelper *channel, const ADCStats &stats) {
  Serial.print(F("ADC_STATS "));
  Serial.println(channel);
  Serial.print(F("ALL "));
  printRange(stats.all);
  Serial.print(F("HIGH_GE_250 "));
  printRange(stats.high);
  Serial.print(F("LOW_LE_150 "));
  printRange(stats.low);
  Serial.print(F("AMBIGUOUS_151_249 count="));
  Serial.println(stats.ambiguous);
}

bool busyHigh = false;
bool dataHigh = false;

bool updateLevel(uint16_t adc, bool previous) {
  if (adc >= HIGH_THRESHOLD) return true;
  if (adc <= LOW_THRESHOLD) return false;
  return previous;
}

void recordEdge(uint8_t channel, bool level, uint32_t timeUs) {
  if (eventCount >= MAX_EVENTS) return;
  events[eventCount++] = {timeUs, channel, uint8_t(level)};
}

void setup() {
  analogReference(DEFAULT);
  Serial.begin(115200);
  while (!Serial) {}

  analogRead(BUSY_PIN);
  analogRead(DATA_PIN);

  Serial.println("Ready. Send 's' to capture for 10 seconds.");
}

void loop() {
  if (!Serial.available() || Serial.read() != 's') return;

  eventCount = 0;
  resetStats(busyStats);
  resetStats(dataStats);

  busyHigh = analogRead(BUSY_PIN) >= HIGH_THRESHOLD;
  dataHigh = analogRead(DATA_PIN) >= HIGH_THRESHOLD;

  Serial.println("CAPTURING");

  const uint32_t start = micros();

  while ((uint32_t)(micros() - start) < CAPTURE_MS * 1000UL &&
         eventCount < MAX_EVENTS) {

    uint16_t busyADC = analogRead(BUSY_PIN);
    uint32_t busyTime = micros();

    uint16_t dataADC = analogRead(DATA_PIN);
    uint32_t dataTime = micros();

    // Statistics use the same readings; no extra ADC conversions or Serial I/O.
    collectStats(busyStats, busyADC);
    collectStats(dataStats, dataADC);

    bool nextBusy = updateLevel(busyADC, busyHigh);
    bool nextData = updateLevel(dataADC, dataHigh);

    if (nextBusy != busyHigh) {
      busyHigh = nextBusy;
      recordEdge(0, busyHigh, busyTime - start);
    }

    if (nextData != dataHigh) {
      dataHigh = nextData;
      recordEdge(1, dataHigh, dataTime - start);
    }
  }

  Serial.println("Time_us,Channel,Level");

  for (uint16_t i = 0; i < eventCount; ++i) {
    Serial.print(events[i].time_us);
    Serial.print(",");
    Serial.print(events[i].channel == 0 ? "BUSY" : "DATA");
    Serial.print(",");
    Serial.println(events[i].level ? "HIGH" : "LOW");
  }

  Serial.print("Events=");
  Serial.println(eventCount);
  printStats(F("BUSY A0"), busyStats);
  printStats(F("DATA A1"), dataStats);
  Serial.println("END");
}

