/*
  Kenwood XS Universal Byte Scanner

  Wiring:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY

  Serial @ 115200:

    n     = send current byte, then advance
    p     = go back one byte and send it
    r     = reset scanner to 00
    xHH   = send arbitrary hex byte
    gHH   = set scanner position to HH

  Examples:
    x7D
    xDE
    x8E
    g80
    n

  Electrical model:
    HIGH = OUTPUT HIGH
    LOW  = INPUT / high-Z
*/

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;


// -----------------------------------------------------------------------------
// Timing measured on KR-V6070
// -----------------------------------------------------------------------------

const uint16_t LEAD_US  = 1150;
const uint16_t START_US = 9800;

const uint16_t ONE_US   = 4900;
const uint16_t ZERO_US  = 9800;

const uint16_t HIGH_US  = 4900;
const uint16_t END_US   = 750;


// -----------------------------------------------------------------------------
// Scanner state
// -----------------------------------------------------------------------------

uint8_t currentByte = 0x00;


// -----------------------------------------------------------------------------
// Bus
// -----------------------------------------------------------------------------

void releaseBus()
{
  pinMode(DATA_TX, INPUT);
  pinMode(BUSY_TX, INPUT);
}


void setData(bool high)
{
  if (high) {
    digitalWrite(DATA_TX, HIGH);
    pinMode(DATA_TX, OUTPUT);
  } else {
    pinMode(DATA_TX, INPUT);
  }
}


void setBusy(bool high)
{
  if (high) {
    digitalWrite(BUSY_TX, HIGH);
    pinMode(BUSY_TX, OUTPUT);
  } else {
    pinMode(BUSY_TX, INPUT);
  }
}


// -----------------------------------------------------------------------------
// XS8 TX
// -----------------------------------------------------------------------------

void sendXS8(uint8_t value)
{
  releaseBus();

  delay(10);


  // BUSY / CTRL HIGH
  setBusy(true);

  delayMicroseconds(LEAD_US);


  // DATA start HIGH
  setData(true);

  delayMicroseconds(START_US);


  // Eight bits, MSB first
  for (int bit = 7; bit >= 0; --bit) {

    setData(false);

    if (value & (1 << bit)) {
      delayMicroseconds(ONE_US);
    } else {
      delayMicroseconds(ZERO_US);
    }


    setData(true);

    delayMicroseconds(HIGH_US);
  }


  // End frame
  setBusy(false);

  delayMicroseconds(END_US);

  setData(false);

  releaseBus();
}


// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

void printHex(uint8_t value)
{
  if (value < 0x10)
    Serial.print('0');

  Serial.print(value, HEX);
}


void printCurrent()
{
  Serial.print("# NEXT = 0x");
  printHex(currentByte);
  Serial.println();
}


void sendAndReport(uint8_t value)
{
  Serial.println();

  Serial.print("========== SEND 0x");
  printHex(value);
  Serial.println(" ==========");

  Serial.flush();

  sendXS8(value);

  Serial.print("# DONE 0x");
  printHex(value);
  Serial.println();
}


// -----------------------------------------------------------------------------
// Hex parser
// -----------------------------------------------------------------------------

int hexValue(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';

  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;

  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;

  return -1;
}


bool readHexByte(uint8_t &value)
{
  unsigned long started = millis();

  while (Serial.available() < 2) {

    if (millis() - started > 2000) {
      Serial.println("# ERROR: expected two hex digits");
      return false;
    }
  }


  char c1 = Serial.read();
  char c2 = Serial.read();

  int hi = hexValue(c1);
  int lo = hexValue(c2);


  if (hi < 0 || lo < 0) {

    Serial.println("# ERROR: invalid hex");

    return false;
  }


  value = (uint8_t)((hi << 4) | lo);

  return true;
}


// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);

  releaseBus();

  Serial.println();
  Serial.println("# Kenwood XS Universal Scanner");
  Serial.println("#");
  Serial.println("# n     next");
  Serial.println("# p     previous");
  Serial.println("# r     reset to 00");
  Serial.println("# xHH   send arbitrary byte");
  Serial.println("# gHH   goto byte");
  Serial.println();

  printCurrent();
}


// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

void loop()
{
  if (!Serial.available())
    return;


  char command = Serial.read();


  if (command == '\r' || command == '\n')
    return;


  // ---------------------------------------------------------------------------
  // NEXT
  // ---------------------------------------------------------------------------

  if (command == 'n' || command == 'N') {

    uint8_t value = currentByte;

    sendAndReport(value);


    // uint8_t naturally wraps FF -> 00
    currentByte++;

    printCurrent();

    return;
  }


  // ---------------------------------------------------------------------------
  // PREVIOUS
  // ---------------------------------------------------------------------------

  if (command == 'p' || command == 'P') {

    currentByte--;

    sendAndReport(currentByte);

    printCurrent();

    return;
  }


  // ---------------------------------------------------------------------------
  // RESET
  // ---------------------------------------------------------------------------

  if (command == 'r' || command == 'R') {

    currentByte = 0x00;

    Serial.println("# RESET");

    printCurrent();

    return;
  }


  // ---------------------------------------------------------------------------
  // EXACT BYTE
  // ---------------------------------------------------------------------------

  if (command == 'x' || command == 'X') {

    uint8_t value;

    if (readHexByte(value)) {
      sendAndReport(value);
    }

    return;
  }


  // ---------------------------------------------------------------------------
  // GOTO
  // ---------------------------------------------------------------------------

  if (command == 'g' || command == 'G') {

    uint8_t value;

    if (readHexByte(value)) {

      currentByte = value;

      printCurrent();
    }

    return;
  }


  // ---------------------------------------------------------------------------
  // Help
  // ---------------------------------------------------------------------------

  if (command == 'h' || command == 'H' || command == '?') {

    Serial.println();
    Serial.println("# n     send next byte");
    Serial.println("# p     previous byte");
    Serial.println("# r     reset to 00");
    Serial.println("# xHH   send exact byte");
    Serial.println("# gHH   set next byte");

    return;
  }


  Serial.print("# Unknown command: ");
  Serial.println(command);
}
