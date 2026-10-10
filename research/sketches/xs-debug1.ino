// Kenwood XS TX + ADC edge capture
//
// D2 -> 2.2k -> DATA
// D3 -> 2.2k -> BUSY
// A0 -> DATA sense
// A5 -> BUSY sense
//
// Serial: x60

const uint8_t DATA_TX  = 2;
const uint8_t BUSY_TX  = 3;
const uint8_t DATA_ADC = A0;
const uint8_t BUSY_ADC = A5;

const int ADC_THRESHOLD = 500;

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
  uint8_t data;
  uint8_t busy;
};

const uint8_t MAX_EDGES = 40;
Edge edges[MAX_EDGES];
uint8_t edgeCount = 0;

int lastData = -1;
int lastBusy = -1;

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

void sampleEdge() {
  uint16_t d = analogRead(DATA_ADC);
  uint16_t b = analogRead(BUSY_ADC);

  int ds = d >= ADC_THRESHOLD;
  int bs = b >= ADC_THRESHOLD;

  if (ds == lastData && bs == lastBusy)
    return;

  lastData = ds;
  lastBusy = bs;

  if (edgeCount >= MAX_EDGES)
    return;

  Edge &e = edges[edgeCount++];

  e.t       = micros();
  e.dataAdc = d;
  e.busyAdc = b;
  e.data    = ds;
  e.busy    = bs;
}

void waitCapture(uint32_t duration) {
  uint32_t start = micros();

  while ((uint32_t)(micros() - start) < duration) {
    sampleEdge();
  }
}

void sendXS(uint8_t value) {
  edgeCount = 0;
  lastData = -1;
  lastBusy = -1;

  releaseBus();

  // Initial idle state
  sampleEdge();
  waitCapture(2000);

  // BUSY high
  driveHigh(BUSY_TX);
  sampleEdge();

  waitCapture(LEAD_US);

  // START
  driveHigh(DATA_TX);
  sampleEdge();

  waitCapture(START_US);

  // 8 bits MSB first
  for (int bit = 7; bit >= 0; --bit) {
    bool one = value & (1 << bit);

    releaseLine(DATA_TX);
    sampleEdge();

    waitCapture(one ? ONE_US : ZERO_US);

    driveHigh(DATA_TX);
    sampleEdge();

    waitCapture(HIGH_US);
  }

  // BUSY falls while DATA stays high
  releaseLine(BUSY_TX);
  sampleEdge();

  waitCapture(END_US);

  // DATA falls
  releaseLine(DATA_TX);
  sampleEdge();

  waitCapture(3000);

  releaseBus();
}

void printCapture() {
  Serial.println(
    F("timestamp_us,data_adc,busy_adc,data_digital,busy_digital")
  );

  for (uint8_t i = 0; i < edgeCount; i++) {
    Edge &e = edges[i];

    Serial.print(e.t);
    Serial.print(',');
    Serial.print(e.dataAdc);
    Serial.print(',');
    Serial.print(e.busyAdc);
    Serial.print(',');
    Serial.print(e.data);
    Serial.print(',');
    Serial.println(e.busy);
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

  Serial.println(F("Kenwood XS TX debug"));
  Serial.println(F("Use xHH, e.g. x60"));
}

void loop() {
  if (!Serial.available())
    return;

  char c = Serial.read();

  if (c != 'x' && c != 'X')
    return;

  while (Serial.available() < 2) {}

  int hi = hexValue(Serial.read());
  int lo = hexValue(Serial.read());

  if (hi < 0 || lo < 0) {
    Serial.println(F("Bad hex"));
    return;
  }

  uint8_t value = (hi << 4) | lo;

  Serial.print(F("# TX 0x"));
  if (value < 0x10) Serial.print('0');
  Serial.println(value, HEX);

  sendXS(value);
  printCapture();
}
