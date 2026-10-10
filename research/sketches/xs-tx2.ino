// ============================================================
// Kenwood XS LIVE TX/RX v4
//
// RX:
//   A0 = DATA / encoded line
//   A5 = BUSY / envelope
//
// TX physical wiring:
//   D2 -- 2.2k --> A0 / DATA
//   D3 -- 2.2k --> A5 / BUSY
//
// IMPORTANT:
//   TX pins NEVER actively drive LOW.
//   LOW = INPUT / high-Z.
//   HIGH = OUTPUT HIGH through external 2.2k.
//
// Real Kenwood timing:
//
//   BUSY rises
//      ~1.0-1.2 ms
//   first DATA transition
//
//   DATA intervals:
//      short ~4.8-5.0 ms
//      long  ~9.6-10.0 ms
//
// Serial commands:
//
//   c = legacy 0x82 CD test
//   t = legacy 0x84 TUNER test
//   s = legacy 0xD6 STOP test
//
//   r = literal replay of captured real CD frame
//
//   j = separator marker
//
// ============================================================


// ============================================================
// PIN MAPPING
// ============================================================

const uint8_t DATA_RX = A0;
const uint8_t BUSY_RX = A5;

const uint8_t DATA_TX = 2;   // D2 -> A0
const uint8_t BUSY_TX = 3;   // D3 -> A5


// ============================================================
// GENERAL TIMING
// ============================================================

const int THRESHOLD = 400;

// BUSY rise -> first DATA edge
const uint32_t START_US = 1150;

// Nominal short interval
const uint32_t T_US = 4900;

// Last DATA edge -> BUSY fall
const uint32_t END_US = 4900;


// ============================================================
// LITERAL CD REPLAY
//
// Stable main CD frame observed repeatedly.
//
// After the initial DATA transition, the measured interval
// sequence was:
//
//   L L S S S S S S
//   S S S S S L S S
//
// where:
//   S ~= 4.9 ms
//   L ~= 9.8 ms
//
// We deliberately replay the waveform rather than interpreting
// these intervals as bytes/bits.
// ============================================================

const uint32_t CD_REPLAY_US[] = {
  9800,
  9800,

  4900,
  4900,
  4900,
  4900,
  4900,
  4900,

  4900,
  4900,
  4900,
  4900,
  4900,

  9800,

  4900,
  4900
};

const uint8_t CD_REPLAY_COUNT =
    sizeof(CD_REPLAY_US) / sizeof(CD_REPLAY_US[0]);


// ============================================================
// RX STATE
// ============================================================

bool dataState = false;
bool busyState = false;

uint32_t captureStartUs = 0;


// ============================================================
// LEGACY 8-BIT TX STATE
// ============================================================

bool txActive = false;

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
// REPLAY STATE
// ============================================================

bool replayActive = false;

uint8_t replayIndex = 0;

enum ReplayPhase {
  REPLAY_IDLE,
  REPLAY_START,
  REPLAY_INTERVALS,
  REPLAY_FINISH
};

ReplayPhase replayPhase = REPLAY_IDLE;

uint32_t replayNextUs = 0;


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


// DATA = encoded line

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


// BUSY = envelope

void setBusy(bool high) {

  if (high) {
    driveHigh(BUSY_TX);
  } else {
    releaseLine(BUSY_TX);
  }
}


// ============================================================
// LEGACY 8-BIT TX
// ============================================================

void startTx(uint8_t value) {

  if (txActive || replayActive) {

    Serial.println("# TX already active");

    return;
  }


  txActive = true;

  txValue = value;
  txBit = 7;


  // Idle state.

  setData(false);
  setBusy(false);


  // Packet envelope starts.

  setBusy(true);


  txPhase = TX_START;

  txNextUs =
      micros() + START_US;
}


// ============================================================
// LEGACY TX STATE MACHINE
// ============================================================

void serviceTx() {

  if (!txActive) {
    return;
  }


  uint32_t now =
      micros();


  if ((int32_t)(now - txNextUs) < 0) {
    return;
  }


  switch (txPhase) {


    // --------------------------------------------------------
    // First DATA transition
    // --------------------------------------------------------

    case TX_START:

      toggleData();

      txPhase = TX_BITS;


      txNextUs =
          now +
          ((((txValue >> txBit) & 1) != 0)
             ? T_US
             : 2 * T_US);

      break;


    // --------------------------------------------------------
    // 8-bit legacy encoding
    //
    // short = 1
    // long  = 0
    // MSB first
    // --------------------------------------------------------

    case TX_BITS:

      toggleData();


      if (txBit == 0) {

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
    // End legacy frame
    // --------------------------------------------------------

    case TX_FINISH:

      setBusy(false);
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
// START LITERAL CD REPLAY
// ============================================================

void startCdReplay() {

  if (txActive || replayActive) {

    Serial.println("# TX already active");

    return;
  }


  Serial.println("# REPLAY real CD frame");


  replayActive = true;

  replayIndex = 0;


  // Ensure idle state first.

  setData(false);
  setBusy(false);


  // Start real packet envelope.

  setBusy(true);


  replayPhase = REPLAY_START;

  replayNextUs =
      micros() + START_US;
}


// ============================================================
// LITERAL REPLAY STATE MACHINE
// ============================================================

void serviceReplay() {

  if (!replayActive) {
    return;
  }


  uint32_t now =
      micros();


  if ((int32_t)(now - replayNextUs) < 0) {
    return;
  }


  switch (replayPhase) {


    // --------------------------------------------------------
    // BUSY has already risen.
    //
    // Generate first DATA transition after START_US.
    // --------------------------------------------------------

    case REPLAY_START:

      toggleData();


      replayIndex = 0;

      replayPhase =
          REPLAY_INTERVALS;


      replayNextUs =
          now +
          CD_REPLAY_US[replayIndex];

      break;


    // --------------------------------------------------------
    // Replay every captured DATA interval literally.
    // --------------------------------------------------------

    case REPLAY_INTERVALS:

      toggleData();


      replayIndex++;


      if (replayIndex >= CD_REPLAY_COUNT) {

        // Last captured DATA transition just happened.

        replayPhase =
            REPLAY_FINISH;


        replayNextUs =
            now + END_US;

      } else {

        replayNextUs =
            now +
            CD_REPLAY_US[replayIndex];
      }

      break;


    // --------------------------------------------------------
    // Finish frame.
    // --------------------------------------------------------

    case REPLAY_FINISH:

      // Envelope ends.

      setBusy(false);


      // Return DATA to idle/high-Z.
      //
      // If DATA is already LOW this changes nothing.
      // If it ended HIGH, this generates the final return
      // to idle after BUSY falls.

      setData(false);


      replayActive = false;

      replayPhase =
          REPLAY_IDLE;


      Serial.println("# REPLAY complete");

      break;


    default:

      break;
  }
}


// ============================================================
// RX / SNIFFER
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


  switch (c) {


    // --------------------------------------------------------
    // Legacy 8-bit CD
    // --------------------------------------------------------

    case 'c':
    case 'C':

      Serial.println("# TX 82 CD");

      startTx(0x82);

      break;


    // --------------------------------------------------------
    // Legacy 8-bit TUNER
    // --------------------------------------------------------

    case 't':
    case 'T':

      Serial.println("# TX 84 TUNER");

      startTx(0x84);

      break;


    // --------------------------------------------------------
    // Legacy 8-bit STOP
    // --------------------------------------------------------

    case 's':
    case 'S':

      Serial.println("# TX D6 STOP");

      startTx(0xD6);

      break;


    // --------------------------------------------------------
    // Literal captured CD waveform
    // --------------------------------------------------------

    case 'r':
    case 'R':

      startCdReplay();

      break;


    // --------------------------------------------------------
    // Manual separator
    // --------------------------------------------------------

    case 'j':
    case 'J':

      Serial.println();
      Serial.println(
        "========== NEXT BUTTON =========="
      );

      break;


    // --------------------------------------------------------

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


  // Never drive the XS bus during boot.

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


  Serial.println(
    "# Kenwood XS LIVE v4"
  );

  Serial.println(
    "# A0=DATA(encoded), A5=BUSY(envelope)"
  );

  Serial.println(
    "# D2=DATA TX, D3=BUSY TX"
  );

  Serial.println(
    "# START=1.15ms, T=4.9ms, END=4.9ms"
  );


  Serial.println(
    "# c = legacy 82 CD"
  );

  Serial.println(
    "# t = legacy 84 TUNER"
  );

  Serial.println(
    "# s = legacy D6 STOP"
  );

  Serial.println(
    "# r = literal replay of captured CD frame"
  );

  Serial.println(
    "# j = separator"
  );
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  captureInputs();

  serviceTx();

  serviceReplay();

  handleSerial();
}
