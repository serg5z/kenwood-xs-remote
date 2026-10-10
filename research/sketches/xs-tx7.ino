/*
  Kenwood XS LIVE v7 — Receiver Input Test

  RX / sniffer:
    A0 = DATA
    A5 = BUSY

  TX:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY

  Electrical:
    HIGH = actively drive HIGH
    LOW  = release line (INPUT / high-Z)

  XS8 wire format:
    BUSY rises
    ~1.15 ms
    DATA rises
    START HIGH ~9.8 ms

    8 bits MSB first:
      bit 1 = DATA LOW ~4.9 ms
      bit 0 = DATA LOW ~9.8 ms
      followed by DATA HIGH ~4.9 ms

    After final HIGH:
      BUSY released
      ~0.75 ms
      DATA released

  Verified receiver input bytes from real front-panel captures:

    TV2    = 0x71
    PHONO  = 0x73
    TAPE1  = 0x79
    TV1    = 0x7A
    TUNER  = 0x7B
    CD     = 0x7D

  Serial commands:

    c = CD
    u = TUNER
    p = PHONO
    a = TAPE1
    1 = TV1
    2 = TV2
    j = separator
*/


// -----------------------------------------------------------------------------
// Pins
// -----------------------------------------------------------------------------

const uint8_t DATA_RX = A0;
const uint8_t BUSY_RX = A5;

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;


// -----------------------------------------------------------------------------
// Sniffer
// -----------------------------------------------------------------------------

const int ADC_THRESHOLD = 500;

int lastData = -1;
int lastBusy = -1;


// -----------------------------------------------------------------------------
// XS timing
// -----------------------------------------------------------------------------

const uint32_t XS_LEAD_US       = 1150;
const uint32_t XS_START_HIGH_US = 9800;

const uint32_t XS_ONE_LOW_US    = 4900;
const uint32_t XS_ZERO_LOW_US   = 9800;
const uint32_t XS_FRAME_HIGH_US = 4900;

const uint32_t XS_END_US        = 750;


// -----------------------------------------------------------------------------
// Line control
// -----------------------------------------------------------------------------

void driveHigh(uint8_t pin)
{
  digitalWrite(pin, HIGH);
  pinMode(pin, OUTPUT);
}


void releaseLine(uint8_t pin)
{
  pinMode(pin, INPUT);
}


void setData(bool high)
{
  digitalWrite(DATA_TX, high ? HIGH : LOW);
  pinMode(DATA_TX, OUTPUT);
}

void setBusy(bool high)
{
  digitalWrite(BUSY_TX, high ? HIGH : LOW);
  pinMode(BUSY_TX, OUTPUT);
}

void releaseData()
{
  pinMode(DATA_TX, INPUT);
}

void releaseBusy()
{
  pinMode(BUSY_TX, INPUT);
}

// -----------------------------------------------------------------------------
// XS8 transmitter
// -----------------------------------------------------------------------------

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


bool xsBusy()
{
  return xsTxActive;
}


void startXsTx(uint8_t value)
{
  if (xsTxActive) {
    Serial.println("# TX busy");
    return;
  }

  xsTxValue = value;
  xsTxBit = 7;

  // Idle DATA released / LOW.
  setData(false);

  // Start BUSY envelope.
  setBusy(true);

  xsTxActive = true;
  xsTxPhase = XS_TX_LEAD;

  xsTxNextUs = micros() + XS_LEAD_US;
}


void serviceXsTx()
{
  if (!xsTxActive)
    return;

  uint32_t now = micros();

  if ((int32_t)(now - xsTxNextUs) < 0)
    return;


  switch (xsTxPhase) {

    // BUSY already HIGH.
    // Raise DATA for start pulse.
    case XS_TX_LEAD:
      setData(true);

      xsTxPhase = XS_TX_START_HIGH;
      xsTxNextUs = now + XS_START_HIGH_US;
      break;


    // Start HIGH finished.
    // Begin bit 7 LOW.
    case XS_TX_START_HIGH:
    {
      setData(false);

      bool bit =
        ((xsTxValue >> xsTxBit) & 1) != 0;

      xsTxPhase = XS_TX_BIT_LOW;

      xsTxNextUs =
        now + (bit ? XS_ONE_LOW_US
                   : XS_ZERO_LOW_US);

      break;
    }


    // Variable LOW portion of bit finished.
    // Raise DATA for fixed HIGH interval.
    case XS_TX_BIT_LOW:
      setData(true);

      xsTxPhase = XS_TX_BIT_HIGH;
      xsTxNextUs = now + XS_FRAME_HIGH_US;
      break;


    // Fixed HIGH interval finished.
    case XS_TX_BIT_HIGH:

      if (xsTxBit == 0) {

        // Final bit complete.
        //
        // Real receiver frames:
        // BUSY falls while DATA remains HIGH.
        setBusy(false);

        xsTxPhase = XS_TX_FINISH;
        xsTxNextUs = now + XS_END_US;

      } else {

        xsTxBit--;

        // Begin next bit LOW.
        setData(false);

        bool bit =
          ((xsTxValue >> xsTxBit) & 1) != 0;

        xsTxPhase = XS_TX_BIT_LOW;

        xsTxNextUs =
          now + (bit ? XS_ONE_LOW_US
                     : XS_ZERO_LOW_US);
      }

      break;


    // Release DATA after BUSY has fallen.
    case XS_TX_FINISH:
      setData(false);

      xsTxActive = false;
      xsTxPhase = XS_TX_IDLE;

      Serial.println("# XS8 TX complete");
      break;


    default:
      break;
  }
}


// -----------------------------------------------------------------------------
// Sniffer
// -----------------------------------------------------------------------------

int readLogic(uint8_t pin)
{
  return analogRead(pin) >= ADC_THRESHOLD ? 1 : 0;
}


void serviceSniffer()
{
  int data = readLogic(DATA_RX);
  int busy = readLogic(BUSY_RX);

  if (data != lastData || busy != lastBusy) {

    Serial.print(micros());
    Serial.print(",");
    Serial.print(data);
    Serial.print(",");
    Serial.println(busy);

    lastData = data;
    lastBusy = busy;
  }
}


// -----------------------------------------------------------------------------
// Input command helper
// -----------------------------------------------------------------------------

void sendInput(const char *name, uint8_t value)
{
  if (xsBusy()) {
    Serial.println("# TX busy");
    return;
  }

  Serial.print("# INPUT ");
  Serial.print(name);
  Serial.print(" -> XS8 0x");

  if (value < 0x10)
    Serial.print("0");

  Serial.println(value, HEX);

  startXsTx(value);
}


// -----------------------------------------------------------------------------
// Serial commands
// -----------------------------------------------------------------------------

void serviceSerial()
{
  while (Serial.available()) {

    char c = Serial.read();

    if (c == '\r' || c == '\n')
      continue;


    switch (c) {

      // CD = 0x7D
      case 'c':
      case 'C':
        sendInput("CD", 0x7D);
        break;


      // TUNER = 0x7B
      case 'u':
      case 'U':
        sendInput("TUNER", 0x7B);
        break;


      // PHONO = 0x73
      case 'p':
      case 'P':
        sendInput("PHONO", 0x73);
        break;


      // TAPE1 = 0x79
      case 'a':
      case 'A':
        sendInput("TAPE1", 0x79);
        break;


      // TV1 = 0x7A
      case '1':
        sendInput("TV1", 0x7A);
        break;


      // TV2 = 0x71
      case '2':
        sendInput("TV2", 0x71);
        break;


      // Visual separator in capture.
      case 'j':
      case 'J':
        Serial.println();
        Serial.println(
          "========== NEXT BUTTON =========="
        );
        break;


      default:
        Serial.print("# Unknown command: ");
        Serial.println(c);
        break;
    }
  }
}


// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);

  pinMode(DATA_RX, INPUT);
  pinMode(BUSY_RX, INPUT);

  releaseLine(DATA_TX);
  releaseLine(BUSY_TX);

  delay(100);

  lastData = readLogic(DATA_RX);
  lastBusy = readLogic(BUSY_RX);


  Serial.println();
  Serial.println("# Kenwood XS LIVE v7");
  Serial.println("# Receiver Input Test");
  Serial.println("#");
  Serial.println("# A0 = DATA RX");
  Serial.println("# A5 = BUSY RX");
  Serial.println("# D2 = DATA TX");
  Serial.println("# D3 = BUSY TX");
  Serial.println("#");

  Serial.println("# Verified captured input bytes:");
  Serial.println("#   TV2    = 71");
  Serial.println("#   PHONO  = 73");
  Serial.println("#   TAPE1  = 79");
  Serial.println("#   TV1    = 7A");
  Serial.println("#   TUNER  = 7B");
  Serial.println("#   CD     = 7D");
  Serial.println("#");

  Serial.println("# Commands:");
  Serial.println("#   c = CD");
  Serial.println("#   u = TUNER");
  Serial.println("#   p = PHONO");
  Serial.println("#   a = TAPE1");
  Serial.println("#   1 = TV1");
  Serial.println("#   2 = TV2");
  Serial.println("#   j = separator");
}


// -----------------------------------------------------------------------------
// Main loop
// -----------------------------------------------------------------------------

void loop()
{
  // TX timing first.
  serviceXsTx();

  // Continue watching the real bus.
  serviceSniffer();

  // Interactive commands.
  serviceSerial();
}
