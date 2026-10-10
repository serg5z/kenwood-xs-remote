// Kenwood XS LIVE TX/RX -- timing test v3
//
// FIXED historical xs-sniffer3 naming:
//
//   A0 = DATA RX
//   A5 = BUSY RX
//
// Physical TX mapping:
//
//   D2 -- 2.2k --> same node as A0 / DATA
//   D3 -- 2.2k --> same node as A5 / BUSY
//
// IMPORTANT OBSERVATION:
//
//   A0 / DATA = encoded transition line
//   A5 / BUSY = packet envelope
//
// Real packet shape:
//
//   BUSY/envelope:
//       ____|===========================|_____
//
//   DATA/encoded:
//       _____|____|________|____|..._________
//
// Timing observed from real Kenwood:
//
//   envelope rise -> first DATA edge ~= 1.1-1.2 ms
//
//   DATA interval:
//       ~4.9 ms = bit 1
//       ~9.8 ms = bit 0
//
//   after last DATA edge -> envelope fall ~= 4.9 ms
//
// TX pins NEVER actively drive LOW.
// LOW = INPUT/high-Z.
// HIGH = OUTPUT HIGH through external 2.2k.
//
// Serial commands:
//
//   c = 0x82
//   t = 0x84
//   s = 0xD6
//
// NOTE:
// This is deliberately still an 8-bit transmitter.
// We have observed longer real frames, but have NOT yet
// established that these commands require a second byte.


// ============================================================
// PIN MAPPING
// ============================================================

const uint8_t DATA_RX = A0;
const uint8_t BUSY_RX = A5;

const uint8_t DATA_TX = 2;   // D2 -> A0 / encoded
const uint8_t BUSY_TX = 3;   // D3 -> A5 / envelope


// ============================================================
// TIMING
// ============================================================

const int THRESHOLD = 400;

// Envelope rise -> first encoded edge
const uint32_t START_US = 1150;

// Basic XS interval
const uint32_t T_US = 4900;

// Last encoded edge -> envelope fall
const uint32_t END_US = 4900;


// ============================================================
// RX STATE
// ============================================================

bool dataState;
bool busyState;

uint32_t captureStartUs;


// ============================================================
// TX STATE
// ============================================================

bool txActive = false;

// Current state of encoded A0/D2 line.
bool txDataState = false;

uint8_t txValue = 0;
int8_t txBit = 7;

uint32_t txNextUs = 0;


enum TxPhase {
  TX_IDLE,
  TX_START,
  TX_BITS,
  TX_FINISH
};

TxPhase txPhase = TX_IDLE;


// ============================================================
// LOW LEVEL LINE CONTROL
// ============================================================

void releaseLine(uint8_t pin) {
  pinMode(pin, INPUT);
}


void driveHigh(uint8_t pin) {
  digitalWrite(pin, HIGH);
  pinMode(pin, OUTPUT);
}


// Encoded DATA line: A0 / D2

void setData(bool high) {

  txDataState = high;

  if (high) {
    driveHigh(DATA_TX);
  } else {
    releaseLine(DATA_TX);
  }
}


void toggleData() {
  setData(!txDataState);
}


// BUSY line is packet envelope: A5 / D3

void setBusy(bool high) {

  if (high) {
    driveHigh(BUSY_TX);
  } else {
    releaseLine(BUSY_TX);
  }
}


// ============================================================
// START TX
// ============================================================

void startTx(uint8_t value) {

  if (txActive) {
    Serial.println("# TX already active");
    return;
  }


  txActive = true;

  txValue = value;
  txBit = 7;


  // Both lines start LOW/high-Z.

  setData(false);
  setBusy(false);


  // Real packet begins with envelope rising.

  setBusy(true);


  // First encoded DATA transition occurs ~1.15 ms later.

  txPhase = TX_START;

  txNextUs =
      micros() + START_US;
}


// ============================================================
// TX STATE MACHINE
// ============================================================

void serviceTx() {

  if (!txActive) {
    return;
  }


  uint32_t now = micros();


  if ((int32_t)(now - txNextUs) < 0) {
    return;
  }


  switch (txPhase) {


    // --------------------------------------------------------
    // Initial encoded transition
    // --------------------------------------------------------

    case TX_START:

      toggleData();

      txPhase = TX_BITS;


      // Schedule interval for bit 7.

      txNextUs =
          now +
          ((((txValue >> txBit) & 1) != 0)
             ? T_US
             : 2 * T_US);

      break;


    // --------------------------------------------------------
    // Eight bits, MSB first
    //
    // 1 = short interval
    // 0 = long interval
    //
    // Each interval terminates in a DATA transition.
    // --------------------------------------------------------

    case TX_BITS:

      toggleData();


      if (txBit == 0) {

        // Last encoded transition has occurred.
        //
        // Real Kenwood leaves the envelope HIGH for
        // approximately one short interval afterwards.

        txPhase = TX_FINISH;

        txNextUs =
            now + END_US;

      } else {

        txBit--;


        bool bit =
            ((txValue >> txBit) & 1) != 0;


        txNextUs =
            now +
            (bit ? T_US : 2 * T_US);
      }

      break;


    // --------------------------------------------------------
    // End frame
    // --------------------------------------------------------

    case TX_FINISH:

      // Real packet envelope ends here.
      setBusy(false);

      // Return encoded line to idle LOW/high-Z.
      setData(false);


      txActive = false;
      txPhase = TX_IDLE;


      Serial.println("# TX complete");

      break;


    default:
      break;
  }
}


// ============================================================
// SNIFFER
// ============================================================

void captureInputs() {

  int dataAdc =
      analogRead(DATA_RX);

  int busyAdc =
      analogRead(BUSY_RX);


  bool newData =
      dataAdc > THRESHOLD;

  bool newBusy =
      busyAdc > THRESHOLD;


  if (newData != dataState ||
      newBusy != busyState) {


    uint32_t t =
        micros() - captureStartUs;


    dataState = newData;
    busyState = newBusy;


    Serial.print(t);
    Serial.print(',');

    Serial.print(dataState);
    Serial.print(',');

    Serial.println(busyState);
  }
}


// ============================================================
// SERIAL COMMANDS
// ============================================================

void handleSerial() {

  if (!Serial.available()) {
    return;
  }


  char c =
      Serial.read();


  // Ignore CR/LF.

  if (c == '\r' ||
      c == '\n') {

    return;
  }


  if (txActive) {

    Serial.println("# TX already active");

    return;
  }


  switch (c) {


    case 'c':
    case 'C':

      Serial.println("# TX 82 CD");

      startTx(0x82);

      break;


    case 't':
    case 'T':

      Serial.println("# TX 84 TUNER");

      startTx(0x84);

      break;


    case 's':
    case 'S':

      Serial.println("# TX D6 STOP");

      startTx(0xD6);

      break;


    default:

      Serial.print("# Unknown command: ");
      Serial.println(c);

      break;
  }
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  while (!Serial) {
  }


  // Never drive XS during startup.

  releaseLine(DATA_TX);
  releaseLine(BUSY_TX);


  delay(100);


  dataState =
      analogRead(DATA_RX) > THRESHOLD;

  busyState =
      analogRead(BUSY_RX) > THRESHOLD;


  captureStartUs =
      micros();


  Serial.println();

  Serial.println("us,DATA,BUSY");

  Serial.print(0);
  Serial.print(',');
  Serial.print(dataState);
  Serial.print(',');
  Serial.println(busyState);


  Serial.println("# Kenwood XS LIVE v3");

  Serial.println(
    "# A0=DATA(encoded), A5=BUSY(envelope)"
  );

  Serial.println(
    "# D2=DATA TX, D3=BUSY TX"
  );

  Serial.println(
    "# START=1.15ms, T=4.9ms, END=4.9ms"
  );

  Serial.println("# c = 82 CD");
  Serial.println("# t = 84 TUNER");
  Serial.println("# s = D6 STOP");
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  captureInputs();

  serviceTx();

  handleSerial();
}
