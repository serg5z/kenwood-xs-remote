/*
  Kenwood XS byte scanner

  D2 -> 2.2k -> DATA
  D3 -> 2.2k -> BUSY

  Serial:
    n = send next byte 0x70..0x7F
    r = reset to 0x70
    xHH = send arbitrary hex byte, e.g. x8E
*/

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;


// Measured KR-V6070 timing
const uint16_t LEAD_US  = 1150;
const uint16_t START_US = 9800;

const uint16_t ONE_US   = 4900;
const uint16_t ZERO_US  = 9800;

const uint16_t HIGH_US  = 4900;
const uint16_t END_US   = 750;


uint8_t scanByte = 0x70;


// -----------------------------------------------------------------------------
// Bus control
//
// Same electrical model as successful 0x7D TX:
//   HIGH = actively driven
//   LOW  = released / high-Z
// -----------------------------------------------------------------------------

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


void releaseBus()
{
  pinMode(DATA_TX, INPUT);
  pinMode(BUSY_TX, INPUT);
}


// -----------------------------------------------------------------------------
// XS8 TX
// -----------------------------------------------------------------------------

void sendXS8(uint8_t value)
{
  releaseBus();

  delay(10);


  // BUSY HIGH
  setBusy(true);

  delayMicroseconds(LEAD_US);


  // DATA HIGH -- start
  setData(true);

  delayMicroseconds(START_US);


  // 8 bits MSB first
  for (int bit = 7; bit >= 0; --bit) {

    // DATA LOW
    setData(false);

    if (value & (1 << bit)) {
      delayMicroseconds(ONE_US);
    } else {
      delayMicroseconds(ZERO_US);
    }


    // DATA HIGH
    setData(true);

    delayMicroseconds(HIGH_US);
  }


  // BUSY LOW while DATA remains HIGH
  setBusy(false);

  delayMicroseconds(END_US);


  // DATA LOW
  setData(false);


  releaseBus();
}


// -----------------------------------------------------------------------------
// Logging
// -----------------------------------------------------------------------------

void printHexByte(uint8_t value)
{
  if (value < 0x10) {
    Serial.print('0');
  }

  Serial.print(value, HEX);
}


void sendAndPrint(uint8_t value)
{
  Serial.print("# SEND 0x");
  printHexByte(value);
  Serial.println();

  Serial.flush();

  sendXS8(value);

  Serial.print("# DONE 0x");
  printHexByte(value);
  Serial.println();
}


// -----------------------------------------------------------------------------
// Hex parser
// -----------------------------------------------------------------------------

int hexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';

  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;

  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;

  return -1;
}


// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);

  releaseBus();

  Serial.println();
  Serial.println("# Kenwood XS byte scanner");
  Serial.println("#");
  Serial.println("# n   = next byte 0x70..0x7F");
  Serial.println("# r   = reset scanner to 0x70");
  Serial.println("# xHH = send arbitrary hex byte");
  Serial.println("#");
  Serial.println("# Scanner ready at 0x70");
}


// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

void loop()
{
  if (!Serial.available()) {
    return;
  }


  char c = Serial.read();


  if (c == '\r' || c == '\n') {
    return;
  }


  // ---------------------------------------------------------------------------
  // NEXT
  // ---------------------------------------------------------------------------

  if (c == 'n' || c == 'N') {

    sendAndPrint(scanByte);

    if (scanByte == 0x7F) {

      Serial.println("# End of 0x70..0x7F range");
      scanByte = 0x70;

    } else {

      scanByte++;
    }

    return;
  }


  // ---------------------------------------------------------------------------
  // RESET
  // ---------------------------------------------------------------------------

  if (c == 'r' || c == 'R') {

    scanByte = 0x70;

    Serial.println("# Scanner reset to 0x70");

    return;
  }


  // ---------------------------------------------------------------------------
  // ARBITRARY HEX: xHH
  // ---------------------------------------------------------------------------

  if (c == 'x' || c == 'X') {

    while (Serial.available() < 2) {
      // Wait for two hex characters.
    }


    char h = Serial.read();
    char l = Serial.read();

    int hi = hexDigit(h);
    int lo = hexDigit(l);


    if (hi < 0 || lo < 0) {

      Serial.println("# Invalid hex");

      return;
    }


    uint8_t value =
      (uint8_t)((hi << 4) | lo);


    sendAndPrint(value);

    return;
  }


  Serial.print("# Unknown command: ");
  Serial.println(c);
}
