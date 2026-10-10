// Kenwood XS8 self-test
//
// RX:
//   A0 = DATA
//   A5 = BUSY
//
// TX:
//   D2 -> 2.2k -> DATA node -> A0
//                         |
//                        47k
//                         |
//                        GND
//
//   D3 -> 2.2k -> BUSY node -> A5
//                         |
//                        47k
//                         |
//                        GND

const uint8_t DATA_PIN = A0;
const uint8_t BUSY_PIN = A5;

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;

const int THRESHOLD = 400;

bool dataState;
bool busyState;
uint32_t startUs;

// ---------- TX ----------

const uint32_t T_US = 4900;

bool txActive = false;
uint8_t txStep = 0;
uint32_t txNextUs = 0;

// START + D6 = 11010110
//
// Each entry is the interval from one DATA rising edge
// to the next, in units of T.
//
// First rising edge is START.
// Then intervals encode the 8 bits.
const uint8_t intervals[] = {
  1, // bit 7 = 1
  1, // bit 6 = 1
  2, // bit 5 = 0
  1, // bit 4 = 1
  2, // bit 3 = 0
  1, // bit 2 = 1
  1, // bit 1 = 1
  2  // bit 0 = 0
};

const uint8_t NUM_INTERVALS =
    sizeof(intervals) / sizeof(intervals[0]);

enum TxPhase {
  TX_IDLE,
  TX_DATA_HIGH,
  TX_DATA_LOW,
  TX_FINISH
};

TxPhase txPhase = TX_IDLE;

void releaseLine(uint8_t pin) {
  pinMode(pin, INPUT);
}

void driveHigh(uint8_t pin) {
  digitalWrite(pin, HIGH);
  pinMode(pin, OUTPUT);
}

void startTx() {
  if (txActive)
    return;

  txActive = true;
  txStep = 0;

  driveHigh(BUSY_TX);

  // START rising edge
  driveHigh(DATA_TX);

  txPhase = TX_DATA_HIGH;
  txNextUs = micros() + T_US;
}

void serviceTx() {
  if (!txActive)
    return;

  uint32_t now = micros();

  if ((int32_t)(now - txNextUs) < 0)
    return;

  switch (txPhase) {

    case TX_DATA_HIGH:
      // End current 4.9 ms DATA pulse
      releaseLine(DATA_TX);

      if (txStep >= NUM_INTERVALS) {
        txPhase = TX_FINISH;
        txNextUs = now + T_US;
        return;
      }

      // For bit 1, next rising edge should occur
      // immediately after the HIGH pulse.
      //
      // For bit 0, wait another T LOW.
      if (intervals[txStep] == 1) {
        driveHigh(DATA_TX);
        txStep++;

        txPhase = TX_DATA_HIGH;
        txNextUs = now + T_US;
      } else {
        txPhase = TX_DATA_LOW;
        txNextUs = now + T_US;
      }

      break;

    case TX_DATA_LOW:
      // Second half of a "0" interval completed.
      driveHigh(DATA_TX);
      txStep++;

      txPhase = TX_DATA_HIGH;
      txNextUs = now + T_US;
      break;

    case TX_FINISH:
      releaseLine(DATA_TX);
      releaseLine(BUSY_TX);

      txActive = false;
      txPhase = TX_IDLE;
      break;

    default:
      break;
  }
}

// ---------- RX ----------

void captureInputs() {
  int dataAdc = analogRead(DATA_PIN);
  int busyAdc = analogRead(BUSY_PIN);

  bool newData = dataAdc > THRESHOLD;
  bool newBusy = busyAdc > THRESHOLD;

  if (newData != dataState || newBusy != busyState) {
    uint32_t t = micros() - startUs;

    dataState = newData;
    busyState = newBusy;

    Serial.print(t);
    Serial.print(',');
    Serial.print(dataState);
    Serial.print(',');
    Serial.println(busyState);
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  releaseLine(DATA_TX);
  releaseLine(BUSY_TX);

  dataState = analogRead(DATA_PIN) > THRESHOLD;
  busyState = analogRead(BUSY_PIN) > THRESHOLD;

  startUs = micros();

  Serial.println("us,DATA,BUSY");
  Serial.print(0);
  Serial.print(',');
  Serial.print(dataState);
  Serial.print(',');
  Serial.println(busyState);

  Serial.println("# Send 's' for one D6 test packet");
}

void loop() {
  captureInputs();
  serviceTx();

  if (Serial.available()) {
    char c = Serial.read();

    Serial.print("# RX char: ");
    Serial.println(c);

    if (!txActive && (c == 's' || c == 'S')) {
      Serial.println("# TX D6");
      startTx();
    }
  }
}
