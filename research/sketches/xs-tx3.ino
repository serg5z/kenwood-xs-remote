// ============================================================
// Kenwood XS LIVE v5
//
// RX:
//   A0 = DATA / encoded line
//   A5 = BUSY / envelope
//
// TX:
//   D2 -- 2.2k --> A0 / DATA
//   D3 -- 2.2k --> A5 / BUSY
//
// Electrical rule:
//   NEVER actively drive LOW.
//   LOW = INPUT / high-Z.
//   HIGH = OUTPUT HIGH through external 2.2k.
//
// ------------------------------------------------------------
// Observed KR-V6070 encoding
//
// BUSY rises.
//
// ~1.1 ms later DATA rises.
//
// DATA start HIGH lasts approximately 9.8 ms.
//
// Then 8 bits, MSB first:
//
//   bit 1:
//      DATA LOW  ~4.9 ms
//      DATA HIGH ~4.9 ms
//
//   bit 0:
//      DATA LOW  ~9.8 ms
//      DATA HIGH ~4.9 ms
//
// After final HIGH:
//      BUSY falls
//      DATA returns LOW
//
// This matches the structure used by KenwoodXS, but the
// KR-V6070 timings observed here are ~4.9/9.8 ms.
//
// ------------------------------------------------------------
// Capture convention:
//
//   CD     = 0x82
//   TUNER  = 0x84
//   PHONO  = 0x8C
//   TAPE1  = 0x86
//   TV1    = 0x85
//   TV2    = 0x8E
//   STOP   = 0xD6
//
// KenwoodXS library uses inverted values, e.g.
//
//   observed CD 0x82
//   ~0x82 = 0x7D
//
// ------------------------------------------------------------
// Serial:
//
//   c = generic XS8 CD     0x82
//   t = generic XS8 TUNER  0x84
//   s = generic XS8 STOP   0xD6
//
//   r = literal known-good CD replay
//
//   j = separator
//
// ============================================================


// ============================================================
// PINS
// ============================================================

const uint8_t DATA_RX = A0;
const uint8_t BUSY_RX = A5;

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;


// ============================================================
// RX
// ============================================================

const int THRESHOLD = 400;

bool dataState = false;
bool busyState = false;

uint32_t captureStartUs = 0;


// ============================================================
// XS TIMING
// ============================================================

// BUSY rise -> DATA start rise
const uint32_t XS_LEAD_US = 1150;

// Initial DATA HIGH start signal
const uint32_t XS_START_HIGH_US = 9800;

// LOW duration representing 1
const uint32_t XS_ONE_LOW_US = 4900;

// LOW duration representing 0
const uint32_t XS_ZERO_LOW_US = 9800;

// Fixed DATA HIGH interval after every bit
const uint32_t XS_FRAME_HIGH_US = 4900;


// ============================================================
// LINE CONTROL
// ============================================================

void releaseLine(uint8_t pin) {

  pinMode(pin, INPUT);
}


void driveHigh(uint8_t pin) {

  digitalWrite(pin, HIGH);
  pinMode(pin, OUTPUT);
}


void setData(bool high) {

  if (high) {
    driveHigh(DATA_TX);
  } else {
    releaseLine(DATA_TX);
  }
}


void setBusy(bool high) {

  if (high) {
    driveHigh(BUSY_TX);
  } else {
    releaseLine(BUSY_TX);
  }
}


// ============================================================
// GENERIC XS8 TX
// ============================================================

enum XsTxPhase {

  XS_TX_IDLE,

  XS_TX_LEAD,

  XS_TX_START_HIGH,

  XS_TX_BIT_LOW,

  XS_TX_BIT_HIGH,

  XS_TX_FINISH
};


bool xsTxActive = false;

XsTxPhase xsTxPhase = XS_TX_IDLE;

uint8_t xsTxValue = 0;

int8_t xsTxBit = 7;

uint32_t xsTxNextUs = 0;


// ============================================================
// START GENERIC XS8 TRANSMISSION
// ============================================================

void startXsTx(uint8_t value) {

  if (xsTxActive) {

    Serial.println("# TX already active");

    return;
  }


  xsTxActive = true;

  xsTxValue = value;

  xsTxBit = 7;


  // Start from idle.

  setData(false);
  setBusy(false);


  // Envelope rises first.

  setBusy(true);


  xsTxPhase = XS_TX_LEAD;

  xsTxNextUs =
      micros() + XS_LEAD_US;
}


// ============================================================
// GENERIC XS8 STATE MACHINE
// ============================================================

void serviceXsTx() {

  if (!xsTxActive) {
    return;
  }


  uint32_t now =
      micros();


  if ((int32_t)(now - xsTxNextUs) < 0) {
    return;
  }


  switch (xsTxPhase) {


    // --------------------------------------------------------
    // BUSY has been HIGH for ~1.15 ms.
    //
    // Raise DATA to begin start signal.
    // --------------------------------------------------------

    case XS_TX_LEAD:

      setData(true);


      xsTxPhase =
          XS_TX_START_HIGH;


      xsTxNextUs =
          now + XS_START_HIGH_US;

      break;


    // --------------------------------------------------------
    // End start HIGH.
    //
    // DATA falls and bit 7 begins.
    // --------------------------------------------------------

    case XS_TX_START_HIGH:
    {

      setData(false);


      bool bit =
          ((xsTxValue >> xsTxBit) & 1) != 0;


      xsTxPhase =
          XS_TX_BIT_LOW;


      xsTxNextUs =
          now +
          (bit
             ? XS_ONE_LOW_US
             : XS_ZERO_LOW_US);

      break;
    }


    // --------------------------------------------------------
    // End encoded LOW pulse.
    //
    // DATA rises for fixed HIGH frame interval.
    // --------------------------------------------------------

    case XS_TX_BIT_LOW:

      setData(true);


      xsTxPhase =
          XS_TX_BIT_HIGH;


      xsTxNextUs =
          now + XS_FRAME_HIGH_US;

      break;


    // --------------------------------------------------------
    // End fixed HIGH interval.
    //
    // Either start next bit or finish packet.
    // --------------------------------------------------------

    case XS_TX_BIT_HIGH:

      if (xsTxBit == 0) {

        // Final HIGH interval completed.
        //
        // Real captures show BUSY dropping while DATA
        // is still HIGH, followed shortly by DATA LOW.

        setBusy(false);


        xsTxPhase =
            XS_TX_FINISH;


        // Observed final DATA return ~0.7-0.8 ms later.

        xsTxNextUs =
            now + 750;

      } else {

        xsTxBit--;


        // Begin next LOW pulse.

        setData(false);


        bool bit =
            ((xsTxValue >> xsTxBit) & 1) != 0;


        xsTxPhase =
            XS_TX_BIT_LOW;


        xsTxNextUs =
            now +
            (bit
               ? XS_ONE_LOW_US
               : XS_ZERO_LOW_US);
      }

      break;


    // --------------------------------------------------------
    // Final DATA release
    // --------------------------------------------------------

    case XS_TX_FINISH:

      setData(false);


      xsTxActive = false;

      xsTxPhase =
          XS_TX_IDLE;


      Serial.println("# XS8 TX complete");

      break;


    default:

      break;
  }
}


// ============================================================
// LITERAL KNOWN-GOOD CD REPLAY
//
// This is the waveform which already caused the KR-V6070
// to switch to CD.
//
// Keep it as an A/B reference.
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
    sizeof(CD_REPLAY_US) /
    sizeof(CD_REPLAY_US[0]);


enum ReplayPhase {

  REPLAY_IDLE,

  REPLAY_LEAD,

  REPLAY_INTERVALS,

  REPLAY_END_BUSY,

  REPLAY_END_DATA
};


bool replayActive = false;

ReplayPhase replayPhase =
    REPLAY_IDLE;

uint8_t replayIndex = 0;

bool replayDataState = false;

uint32_t replayNextUs = 0;


// ============================================================
// REPLAY DATA CONTROL
// ============================================================

void setReplayData(bool high) {

  replayDataState = high;

  setData(high);
}


void toggleReplayData() {

  setReplayData(
      !replayDataState
  );
}


// ============================================================
// START LITERAL CD REPLAY
// ============================================================

void startCdReplay() {

  if (xsTxActive ||
      replayActive) {

    Serial.println("# TX already active");

    return;
  }


  Serial.println(
    "# REPLAY known-good CD"
  );


  replayActive = true;

  replayIndex = 0;

  replayDataState = false;


  setData(false);
  setBusy(false);


  // Envelope starts.

  setBusy(true);


  replayPhase =
      REPLAY_LEAD;


  replayNextUs =
      micros() + XS_LEAD_US;
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
    // Initial DATA edge
    // --------------------------------------------------------

    case REPLAY_LEAD:

      toggleReplayData();


      replayIndex = 0;


      replayPhase =
          REPLAY_INTERVALS;


      replayNextUs =
          now +
          CD_REPLAY_US[replayIndex];

      break;


    // --------------------------------------------------------
    // Literal DATA transitions
    // --------------------------------------------------------

    case REPLAY_INTERVALS:

      toggleReplayData();


      replayIndex++;


      if (replayIndex >=
          CD_REPLAY_COUNT) {

        // The captured CD waveform ends with DATA HIGH.

        replayPhase =
            REPLAY_END_BUSY;


        replayNextUs =
            now + XS_FRAME_HIGH_US;

      } else {

        replayNextUs =
            now +
            CD_REPLAY_US[replayIndex];
      }

      break;


    // --------------------------------------------------------
    // BUSY falls first
    // --------------------------------------------------------

    case REPLAY_END_BUSY:

      setBusy(false);


      replayPhase =
          REPLAY_END_DATA;


      replayNextUs =
          now + 750;

      break;


    // --------------------------------------------------------
    // DATA returns LOW
    // --------------------------------------------------------

    case REPLAY_END_DATA:

      setReplayData(false);


      replayActive = false;

      replayPhase =
          REPLAY_IDLE;


      Serial.println(
        "# REPLAY complete"
      );

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
// SERIAL
// ============================================================

void handleSerial() {

  if (!Serial.available()) {
    return;
  }


  char c =
      Serial.read();


  if (c == '\r' ||
      c == '\n') {

    return;
  }


  switch (c) {


    // --------------------------------------------------------
    // Generic XS8 CD
    // --------------------------------------------------------

    case 'c':
    case 'C':

  Serial.println("# XS8 TX 7D CD");

      startXsTx(0x7D);

      break;


    // --------------------------------------------------------
    // Generic XS8 TUNER
    // --------------------------------------------------------

    case 't':
    case 'T':

  Serial.println("# XS8 TX 7B TUNER");
  startXsTx(0x7B);
      break;


    // --------------------------------------------------------
    // Generic XS8 STOP
    // --------------------------------------------------------

    case 's':
    case 'S':

  Serial.println("# XS8 TX 29 STOP");
  startXsTx(0x29);

      break;


    // --------------------------------------------------------
    // Known-good literal CD replay
    // --------------------------------------------------------

    case 'r':
    case 'R':

      startCdReplay();

      break;


    // --------------------------------------------------------
    // Separator
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

      Serial.print(
        "# Unknown command: "
      );

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


  // XS must remain undriven during boot.

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

  Serial.println(
    "us,DATA,BUSY"
  );


  Serial.print(0);
  Serial.print(',');

  Serial.print(dataState);
  Serial.print(',');

  Serial.println(busyState);


  Serial.println(
    "# Kenwood XS LIVE v5"
  );


  Serial.println(
    "# A0=DATA, A5=BUSY"
  );

  Serial.println(
    "# D2=DATA TX, D3=BUSY TX"
  );


  Serial.println(
    "# XS8: LOW pulse encodes bit"
  );

  Serial.println(
    "# 1=4.9ms LOW, 0=9.8ms LOW"
  );

  Serial.println(
    "# fixed HIGH=4.9ms"
  );


  Serial.println(
    "# c = generic XS8 82 CD"
  );

  Serial.println(
    "# t = generic XS8 84 TUNER"
  );

  Serial.println(
    "# s = generic XS8 D6 STOP"
  );

  Serial.println(
    "# r = known-good literal CD replay"
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

  serviceXsTx();

  serviceReplay();

  handleSerial();
}
