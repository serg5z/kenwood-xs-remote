// Kenwood XS TX + edge snapshots
//
// Wiring:
//   D2 -> 2.2k -> DATA
//   D3 -> 2.2k -> BUSY
//   A0 --------> DATA sense
//   A5 --------> BUSY sense
//
// Serial:
//   x60
//
// Captures ADC immediately after every TX transition,
// then prints everything only AFTER the frame is complete.

const uint8_t DATA_TX  = 2;
const uint8_t BUSY_TX  = 3;
const uint8_t DATA_ADC = A0;
const uint8_t BUSY_ADC = A5;

const uint16_t ADC_THRESHOLD = 500;

// Our previously working XS timing
const uint16_t LEAD_US  = 1150;
const uint16_t START_US = 9800;
const uint16_t ONE_US   = 4900;
const uint16_t ZERO_US  = 9800;
const uint16_t HIGH_US  = 4900;
const uint16_t END_US   = 750;

struct Edge {
  uint32_t t;
  uint16_t dataAdc;
  uint16_t busyAdc;
};

const uint8_t MAX_EDGES = 24;
Edge edges[MAX_EDGES];
uint8_t edgeCount = 0;

void releaseLine(uint8_t pin) {
  pinMode(pin, INPUT);
}

void driveHigh(uint8_t pin) {
  digitalWrite(pin, HIGH);
  pinMode(pin, OUTPUT);
}

void releaseBus() {
  releaseLine(DATA_TX);
  releaseLine(BUSY_TX);
}

// Snapshot only.
// Called once after an intentional transition.
void snapshot() {
  if (edgeCount >= MAX_EDGES)
    return;

  Edge &e = edges[edgeCount++];

  e.t = micros();
  e.dataAdc = analogRead(DATA_ADC);
  e.busyAdc = analogRead(BUSY_ADC);
}

void sendXS(uint8_t value) {
  edgeCount = 0;

  // Establish idle.
  releaseBus();
  delayMicroseconds(2000);
  snapshot();

  // BUSY ↑
  driveHigh(BUSY_TX);
  snapshot();
  delayMicroseconds(LEAD_US);

  // DATA ↑ -- start pulse
  driveHigh(DATA_TX);
  snapshot();
  delayMicroseconds(START_US);

  // 8 bits, MSB first.
  for (int bit = 7; bit >= 0; --bit) {
    bool one = (value & (1 << bit)) != 0;

    // DATA ↓
    releaseLine(DATA_TX);
    snapshot();

    delayMicroseconds(one ? ONE_US : ZERO_US);

    // DATA ↑
    driveHigh(DATA_TX);
    snapshot();

    delayMicroseconds(HIGH_US);
  }

  // End of frame:
  // BUSY ↓ while DATA remains HIGH.
  releaseLine(BUSY_TX);
  snapshot();

  delayMicroseconds(END_US);

  // DATA ↓
  releaseLine(DATA_TX);
  snapshot();

  // Ensure everything released.
  releaseBus();
}

void printCapture() {
  Serial.println(
    F("timestamp_us,data_adc,busy_adc,data_digital,busy_digital")
  );

  for (uint8_t i = 0; i < edgeCount; ++i) {
    const Edge &e = edges[i];

    Serial.print(e.t);
    Serial.print(',');
    Serial.print(e.dataAdc);
    Serial.print(',');
    Serial.print(e.busyAdc);
    Serial.print(',');
    Serial.print(e.dataAdc >= ADC_THRESHOLD ? 1 : 0);
    Serial.print(',');
    Serial.println(e.busyAdc >= ADC_THRESHOLD ? 1 : 0);
  }

  Serial.println(F("# END"));
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

void setup() {
  Serial.begin(115200);

  releaseBus();

  delay(1000);

  Serial.println(F("Kenwood XS TX edge debug"));
  Serial.println(F("Use xHH, e.g. x60"));
}

void loop() {
  static char buf[16];
  static uint8_t pos = 0;

  while (Serial.available()) {
    char c = Serial.read();

    // Echo so we can SEE that Arduino receives input.
    Serial.write(c);

    if (c == '\r' || c == '\n') {
      if (pos == 0)
        continue;

      buf[pos] = '\0';

      Serial.println();
      Serial.print(F("# GOT: "));
      Serial.println(buf);

      if (pos == 3 && (buf[0] == 'x' || buf[0] == 'X')) {
        int hi = hexValue(buf[1]);
        int lo = hexValue(buf[2]);

        if (hi >= 0 && lo >= 0) {
          uint8_t value = (hi << 4) | lo;

          Serial.print(F("# TX 0x"));
          if (value < 0x10)
            Serial.print('0');
          Serial.println(value, HEX);

          sendXS(value);
          printCapture();
        } else {
          Serial.println(F("# BAD HEX"));
        }
      } else {
        Serial.println(F("# Use xHH, e.g. x60"));
      }

      pos = 0;
      continue;
    }

    if (pos < sizeof(buf) - 1)
      buf[pos++] = c;
  }
}
