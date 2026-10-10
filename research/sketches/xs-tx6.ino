/*
  Kenwood XS LIVE v6

  RX/sniffer:
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

  Commands:
    p = PLAY:  DE -> ~2.1 ms gap -> 7D
    a = PAUSE: D2
    s = STOP:  D6
    j = separator

  Real observed CD transport:
    PLAY  = DE -> 7D
    PAUSE = D2
    STOP  = D6
*/

const uint8_t DATA_RX = A0;
const uint8_t BUSY_RX = A5;

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;


// -----------------------------------------------------------------------------
// ADC sniffer
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

// Observed gap:
// end of first frame -> BUSY rise of second frame
// ~2.1 ms
const uint32_t XS_PLAY_GAP_US   = 2100;


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
  if (high)
    driveHigh(DATA_TX);
  else
    releaseLine(DATA_TX);
}

void setBusy(bool high)
{
  if (high)
    driveHigh(BUSY_TX);
  else
    releaseLine(BUSY_TX);
}


// -----------------------------------------------------------------------------
// XS8 TX state machine
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
  if (xsTxActive)
    return;

  xsTxValue = value;
  xsTxBit = 7;

  // Idle DATA stays released.
  setData(false);

  // Start packet envelope.
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

    // BUSY has already risen.
    // Now raise DATA for the start signal.
    case XS_TX_LEAD:
      setData(true);

      xsTxPhase = XS_TX_START_HIGH;
      xsTxNextUs = now + XS_START_HIGH_US;
      break;


    // Start signal complete.
    // Begin bit 7 with DATA LOW.
    case XS_TX_START_HIGH:
    {
      setData(false);

      bool bit = ((xsTxValue >> xsTxBit) & 1) != 0;

      xsTxPhase = XS_TX_BIT_LOW;

      xsTxNextUs =
        now + (bit ? XS_ONE_LOW_US : XS_ZERO_LOW_US);

      break;
    }


    // LOW pulse complete.
    // Every bit gets a fixed HIGH phase.
    case XS_TX_BIT_LOW:
      setData(true);

      xsTxPhase = XS_TX_BIT_HIGH;
      xsTxNextUs = now + XS_FRAME_HIGH_US;
      break;


    // Fixed HIGH complete.
    case XS_TX_BIT_HIGH:

      if (xsTxBit == 0) {

        // Last bit finished.
        // Real captures show BUSY falling while DATA
        // is still HIGH.
        setBusy(false);

        xsTxPhase = XS_TX_FINISH;
        xsTxNextUs = now + XS_END_US;

      } else {

        xsTxBit--;

        // Start next bit LOW.
        setData(false);

        bool bit =
          ((xsTxValue >> xsTxBit) & 1) != 0;

        xsTxPhase = XS_TX_BIT_LOW;

        xsTxNextUs =
          now + (bit ? XS_ONE_LOW_US : XS_ZERO_LOW_US);
      }

      break;


    // Finally release DATA.
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
// PLAY sequence DE -> 7D
// -----------------------------------------------------------------------------

enum PlayPhase {
  PLAY_IDLE,
  PLAY_SEND_DE,
  PLAY_GAP,
  PLAY_SEND_7D
};

PlayPhase playPhase = PLAY_IDLE;

uint32_t playNextUs = 0;


void startPlay()
{
  if (playPhase != PLAY_IDLE || xsBusy())
    return;

  Serial.println("# PLAY: DE -> 7D");

  Serial.println("# XS8 TX DE");
  startXsTx(0xDE);

  playPhase = PLAY_SEND_DE;
}


void servicePlay()
{
  uint32_t now = micros();

  switch (playPhase) {

    case PLAY_IDLE:
      break;


    // Wait for DE frame to finish completely.
    case PLAY_SEND_DE:
      if (!xsBusy()) {
        playNextUs = now + XS_PLAY_GAP_US;
        playPhase = PLAY_GAP;
      }
      break;


    // Observed ~2.1 ms between frames.
    case PLAY_GAP:
      if ((int32_t)(now - playNextUs) >= 0) {

        Serial.println("# XS8 TX 7D");
        startXsTx(0x7D);

        playPhase = PLAY_SEND_7D;
      }
      break;


    // Wait for 7D to complete.
    case PLAY_SEND_7D:
      if (!xsBusy()) {
        Serial.println("# PLAY sequence complete");
        playPhase = PLAY_IDLE;
      }
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
// Serial commands
// -----------------------------------------------------------------------------

void serviceSerial()
{
  while (Serial.available()) {

    char c = Serial.read();

    if (c == '\r' || c == '\n')
      continue;

    switch (c) {

      case 'p':
      case 'P':
        if (!xsBusy() && playPhase == PLAY_IDLE) {
          startPlay();
        }
        break;


      case 'a':
      case 'A':
        if (!xsBusy() && playPhase == PLAY_IDLE) {
          Serial.println("# XS8 TX D2 PAUSE");
          startXsTx(0xD2);
        }
        break;


      case 's':
      case 'S':
        if (!xsBusy() && playPhase == PLAY_IDLE) {
          Serial.println("# XS8 TX D6 STOP");
          startXsTx(0xD6);
        }
        break;


      case 'j':
      case 'J':
        Serial.println();
        Serial.println("========== NEXT BUTTON ==========");
        break;

case 'd':
case 'D':
  Serial.println("# XS8 TX DE only");
  startXsTx(0xDE);
  break;

case '7':
  Serial.println("# XS8 TX 7D only");
  startXsTx(0x7D);
  break;


      default:
        Serial.print("# Unknown command: ");
        Serial.println(c);
        break;
    }
  }
}


// -----------------------------------------------------------------------------
// Setup / loop
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
  Serial.println("# Kenwood XS LIVE v6");
  Serial.println("# A0=DATA, A5=BUSY");
  Serial.println("# D2=DATA TX, D3=BUSY TX");
  Serial.println("#");
  Serial.println("# Real observed CD transport bytes:");
  Serial.println("# PLAY  = DE -> 7D");
  Serial.println("# PAUSE = D2");
  Serial.println("# STOP  = D6");
  Serial.println("#");
  Serial.println("# p = PLAY");
  Serial.println("# a = PAUSE");
  Serial.println("# s = STOP");
  Serial.println("# j = separator");
}


void loop()
{
  // Keep TX timing serviced first.
  serviceXsTx();

  // PLAY may start a second XS frame.
  servicePlay();

  serviceSniffer();
  serviceSerial();
}
