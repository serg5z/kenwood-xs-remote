/*
  Kenwood XS LIVE v9
  Active-LOW TX experiment

  RX:
    A0 = DATA
    A5 = BUSY

  TX:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY

  IMPORTANT:
    During TX frame:
      HIGH = OUTPUT HIGH
      LOW  = OUTPUT LOW

    Outside TX:
      both TX pins = INPUT / high-Z

  Commands:
    1 = TV1   0x7A
    2 = TV2   0x71

    c = CD    0x7D
    u = TUNER 0x7B
    p = PHONO 0x73
    a = TAPE1 0x79

    j = separator
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
//
// Based primarily on our real KR-V6070 captures.
// -----------------------------------------------------------------------------

const uint16_t XS_LEAD_US       = 1200;
const uint16_t XS_START_HIGH_US = 9720;

const uint16_t XS_ONE_LOW_US    = 4800;
const uint16_t XS_ZERO_LOW_US   = 9720;

const uint16_t XS_HIGH_US       = 4900;

const uint16_t XS_END_US        = 750;


// -----------------------------------------------------------------------------
// Active line control
// -----------------------------------------------------------------------------

void driveData(bool high)
{
  digitalWrite(DATA_TX, high ? HIGH : LOW);
  pinMode(DATA_TX, OUTPUT);
}


void driveBusy(bool high)
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


void releaseBus()
{
  releaseData();
  releaseBusy();
}


// -----------------------------------------------------------------------------
// Blocking XS8 transmitter
// -----------------------------------------------------------------------------

void sendXs8Blocking(uint8_t value)
{
  /*
     Idle bus belongs to Kenwood.
     Our MCU starts completely high-Z.
  */

  releaseBus();

  delayMicroseconds(1000);


  /*
     Begin actively driven frame.

     Initial state:
       DATA LOW
       BUSY HIGH
  */

  driveData(false);
  driveBusy(true);

  delayMicroseconds(XS_LEAD_US);


  /*
     Start pulse:
       DATA HIGH
       BUSY HIGH
  */

  driveData(true);

  delayMicroseconds(XS_START_HIGH_US);


  /*
     Eight bits, MSB first.

       1 = DATA LOW ~4.8 ms
       0 = DATA LOW ~9.72 ms

     Every bit is followed by:
       DATA HIGH ~4.9 ms
  */

  for (int bit = 7; bit >= 0; --bit) {

    driveData(false);

    if (value & (1 << bit)) {
      delayMicroseconds(XS_ONE_LOW_US);
    } else {
      delayMicroseconds(XS_ZERO_LOW_US);
    }


    driveData(true);

    delayMicroseconds(XS_HIGH_US);
  }


  /*
     End frame:

       DATA remains HIGH
       BUSY goes LOW
  */

  driveBusy(false);

  delayMicroseconds(XS_END_US);


  /*
     DATA goes LOW.
  */

  driveData(false);


  /*
     Give LOW state a tiny settling interval before
     relinquishing both lines.

     This is NOT part of our decoded protocol timing;
     it just prevents LOW -> high-Z from happening
     effectively simultaneously in software.
  */

  delayMicroseconds(50);


  /*
     Relinquish shared bus.
  */

  releaseBus();
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
// TX helper
// -----------------------------------------------------------------------------

void sendCommand(const char *name, uint8_t value)
{
  Serial.print("# ACTIVE TX ");
  Serial.print(name);
  Serial.print(" -> 0x");

  if (value < 0x10) {
    Serial.print("0");
  }

  Serial.println(value, HEX);

  Serial.flush();


  /*
     serviceSniffer() cannot run during this call.
     That's intentional for this experiment.
  */

  sendXs8Blocking(value);


  /*
     Resync sniffer state after blocking TX so that
     our own final transition doesn't confuse it.
  */

  lastData = readLogic(DATA_RX);
  lastBusy = readLogic(BUSY_RX);

  Serial.println("# TX complete");
}


// -----------------------------------------------------------------------------
// Serial command interface
// -----------------------------------------------------------------------------

void serviceSerial()
{
  while (Serial.available()) {

    char c = Serial.read();

    if (c == '\r' || c == '\n') {
      continue;
    }

    switch (c) {

      case '1':
        sendCommand("TV1", 0x7A);
        break;


      case '2':
        sendCommand("TV2", 0x71);
        break;


      case 'c':
      case 'C':
        sendCommand("CD", 0x7D);
        break;


      case 'u':
      case 'U':
        sendCommand("TUNER", 0x7B);
        break;


      case 'p':
      case 'P':
        sendCommand("PHONO", 0x73);
        break;


      case 'a':
      case 'A':
        sendCommand("TAPE1", 0x79);
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


  // RX pins
  pinMode(DATA_RX, INPUT);
  pinMode(BUSY_RX, INPUT);


  // Absolutely do not own the XS bus at startup.
  releaseBus();


  delay(100);


  lastData = readLogic(DATA_RX);
  lastBusy = readLogic(BUSY_RX);


  Serial.println();
  Serial.println("# Kenwood XS LIVE v9");
  Serial.println("# Active-LOW blocking TX experiment");
  Serial.println("#");
  Serial.println("# TX wiring:");
  Serial.println("#   D2 -- 2.2k -- DATA");
  Serial.println("#   D3 -- 2.2k -- BUSY");
  Serial.println("#");
  Serial.println("# 1 = TV1   0x7A");
  Serial.println("# 2 = TV2   0x71");
  Serial.println("# c = CD    0x7D");
  Serial.println("# u = TUNER 0x7B");
  Serial.println("# p = PHONO 0x73");
  Serial.println("# a = TAPE1 0x79");
  Serial.println("# j = separator");
  Serial.println("#");
  Serial.println("# Idle TX pins are high-Z.");
}


// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

void loop()
{
  serviceSniffer();
  serviceSerial();
}
