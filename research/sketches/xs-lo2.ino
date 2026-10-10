// Kenwood XS8 TX/RX bench test
//
// RX:
//   A0 = DATA
//   A5 = BUSY
//
// TX:
//   D2 -> 2.2k -> DATA node -> A0 -> 47k -> GND
//   D3 -> 2.2k -> BUSY node -> A5 -> 47k -> GND
//
// NO KENWOOD CONNECTED.

const uint8_t DATA_PIN = A5;
const uint8_t BUSY_PIN = A0;

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;

const int THRESHOLD = 400;

const uint32_t T_US = 4900;

bool dataState;
bool busyState;
uint32_t startUs;

// ---------- TX ----------

bool txActive = false;
bool txDataState = false;

uint8_t txBit = 0;
uint8_t txValue = 0;

uint32_t txNextUs = 0;

enum TxPhase {
  TX_IDLE,
  TX_START,
  TX_BITS,
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

void setTxData(bool high) {
  txDataState = high;

  if (high) {
    driveHigh(DATA_TX);
  } else {
    releaseLine(DATA_TX);
  }
}

void toggleTxData() {
  setTxData(!txDataState);
}

void startTx(uint8_t value) {
  if (txActive)
    return;

  txActive = true;
  txValue = value;
  txBit = 7;

  // Both lines initially LOW/released.
  setTxData(false);

  // BUSY goes HIGH first.
  driveHigh(BUSY_TX);

  // Start/framing: wait one base interval,
  // then first DATA transition.
  txPhase = TX_START;
  txNextUs = micros() + T_US;
}

void serviceTx() {
  if (!txActive)
    return;

  uint32_t now = micros();

  if ((int32_t)(now - txNextUs) < 0)
    return;

  switch (txPhase) {

    case TX_START:
      // Initial framing transition.
      toggleTxData();

      txPhase = TX_BITS;

      // Schedule first data-bit interval.
      txNextUs = now +
          (((txValue >> txBit) & 1) ? T_US : 2 * T_US);

      break;

    case TX_BITS:
      // End of current bit interval = DATA transition.
      toggleTxData();

      if (txBit == 0) {
        // All 8 bits sent.
        txPhase = TX_FINISH;
        txNextUs = now + T_US;
      } else {
        txBit--;

        bool bit = (txValue >> txBit) & 1;

        txNextUs = now + (bit ? T_US : 2 * T_US);
      }

      break;

    case TX_FINISH:
      // Return everything to idle.
      setTxData(false);
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

  Serial.println("# Send 's' for D6");
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
      startTx(0xD6);
    }
  }
}
