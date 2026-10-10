/*
  Kenwood XS LIVE v8
  Blocking timing test — TV1 / TV2 only

  RX:
    A0 = DATA
    A5 = BUSY

  TX:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY

  Commands:
    1 = TV1 = 0x7A
    2 = TV2 = 0x71
    j = separator

  Timing derived from fresh real receiver captures.
*/

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
// Timing
// -----------------------------------------------------------------------------

const uint16_t XS_LEAD_US       = 1200;
const uint16_t XS_START_HIGH_US = 9720;

const uint16_t XS_ONE_LOW_US    = 4800;
const uint16_t XS_ZERO_LOW_US   = 9720;

const uint16_t XS_HIGH_US       = 4900;

const uint16_t XS_END_US        = 750;


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
// Blocking XS8 transmitter
// -----------------------------------------------------------------------------

void sendXs8Blocking(uint8_t value)
{
  // Make sure both lines begin released.
  setData(false);
  setBusy(false);

  delayMicroseconds(1000);


  // BUSY rises.
  setBusy(true);

  delayMicroseconds(XS_LEAD_US);


  // DATA rises: start signal.
  setData(true);

  delayMicroseconds(XS_START_HIGH_US);


  // 8 bits, MSB first.
  for (int bit = 7; bit >= 0; --bit) {

    // Variable LOW pulse.
    setData(false);

    if (value & (1 << bit)) {
      delayMicroseconds(XS_ONE_LOW_US);
    } else {
      delayMicroseconds(XS_ZERO_LOW_US);
    }


    // Fixed HIGH interval.
    setData(true);

    delayMicroseconds(XS_HIGH_US);
  }


  // BUSY falls while DATA remains HIGH.
  setBusy(false);

  delayMicroseconds(XS_END_US);


  // DATA falls / releases.
  setData(false);
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
// Send helper
// -----------------------------------------------------------------------------

void sendInput(const char *name, uint8_t value)
{
  Serial.print("# BLOCKING TX ");
  Serial.print(name);
  Serial.print(" -> 0x");

  if (value < 0x10)
    Serial.print("0");

  Serial.println(value, HEX);

  Serial.flush();

  sendXs8Blocking(value);

  Serial.println("# TX complete");
}


// -----------------------------------------------------------------------------
// Serial
// -----------------------------------------------------------------------------

void serviceSerial()
{
  while (Serial.available()) {

    char c = Serial.read();

    if (c == '\r' || c == '\n')
      continue;

    switch (c) {

      case '1':
        sendInput("TV1", 0x7A);
        break;

      case '2':
        sendInput("TV2", 0x71);
        break;

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
  Serial.println("# Kenwood XS LIVE v8");
  Serial.println("# Blocking timing test");
  Serial.println("#");
  Serial.println("# 1 = TV1  0x7A");
  Serial.println("# 2 = TV2  0x71");
  Serial.println("# j = separator");
}


// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

void loop()
{
  serviceSniffer();
  serviceSerial();
}
