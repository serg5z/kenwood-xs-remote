/*
  Kenwood XS Universal Scanner — line parser

  Wiring:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY

  Serial: 115200

  Commands:
    n       send current byte, advance
    p       go back one byte, send
    r       reset scanner to 00
    xB5     send exact byte B5
    xDE     send exact byte DE
    g80     set scanner position to 80
    ?       help

  Works with:
    No line ending
    Newline
    Carriage return
    Both NL & CR

  For "No line ending", xHH/gHH execute as soon
  as all 3 characters have arrived.
*/

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;


// -----------------------------------------------------------------------------
// KR-V6070 measured timing
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
// Command buffer
// -----------------------------------------------------------------------------

char cmd[8];
uint8_t cmdLen = 0;


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
// XS8 transmitter
// -----------------------------------------------------------------------------

void sendXS8(uint8_t value)
{
  releaseBus();

  delay(10);

  // BUSY rises
  setBusy(true);

  delayMicroseconds(LEAD_US);

  // DATA start
  setData(true);

  delayMicroseconds(START_US);

  // Eight bits, MSB first
  for (int bit = 7; bit >= 0; --bit) {

    setData(false);

    if (value & (1 << bit))
      delayMicroseconds(ONE_US);
    else
      delayMicroseconds(ZERO_US);

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


bool parseHexByte(char hiChar, char loChar, uint8_t &value)
{
  int hi = hexDigit(hiChar);
  int lo = hexDigit(loChar);

  if (hi < 0 || lo < 0)
    return false;

  value = (uint8_t)((hi << 4) | lo);

  return true;
}


// -----------------------------------------------------------------------------
// Execute command
// -----------------------------------------------------------------------------

void executeCommand()
{
  if (cmdLen == 0)
    return;


  // ---------------------------------------------------------------------------
  // Single-character commands
  // ---------------------------------------------------------------------------

  if (cmdLen == 1) {

    char c = cmd[0];


    if (c == 'n' || c == 'N') {

      uint8_t value = currentByte;

      sendAndReport(value);

      currentByte++;

      printCurrent();

      return;
    }


    if (c == 'p' || c == 'P') {

      currentByte--;

      sendAndReport(currentByte);

      printCurrent();

      return;
    }


    if (c == 'r' || c == 'R') {

      currentByte = 0x00;

      Serial.println("# RESET");

      printCurrent();

      return;
    }


    if (c == '?') {

      Serial.println();
      Serial.println("# Commands:");
      Serial.println("#   n     send next");
      Serial.println("#   p     previous");
      Serial.println("#   r     reset to 00");
      Serial.println("#   xHH   send exact byte");
      Serial.println("#   gHH   set scanner position");
      Serial.println();

      return;
    }
  }


  // ---------------------------------------------------------------------------
  // xHH / gHH
  // ---------------------------------------------------------------------------

  if (cmdLen == 3) {

    char operation = cmd[0];

    uint8_t value;

    if (!parseHexByte(cmd[1], cmd[2], value)) {

      Serial.print("# ERROR: invalid hex: ");

      for (uint8_t i = 0; i < cmdLen; i++)
        Serial.print(cmd[i]);

      Serial.println();

      return;
    }


    // xHH
    if (operation == 'x' || operation == 'X') {

      sendAndReport(value);

      return;
    }


    // gHH
    if (operation == 'g' || operation == 'G') {

      currentByte = value;

      printCurrent();

      return;
    }
  }


  Serial.print("# ERROR: unknown command: ");

  for (uint8_t i = 0; i < cmdLen; i++)
    Serial.print(cmd[i]);

  Serial.println();
}


// -----------------------------------------------------------------------------
// Determine whether buffered command is complete
// -----------------------------------------------------------------------------

bool commandComplete()
{
  if (cmdLen == 0)
    return false;


  char first = cmd[0];


  // These commands consist of exactly one character.
  if (first == 'n' || first == 'N' ||
      first == 'p' || first == 'P' ||
      first == 'r' || first == 'R' ||
      first == '?') {

    return cmdLen >= 1;
  }


  // xHH and gHH consist of exactly three characters.
  if (first == 'x' || first == 'X' ||
      first == 'g' || first == 'G') {

    return cmdLen >= 3;
  }


  return false;
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
  Serial.println("# n     next byte");
  Serial.println("# p     previous byte");
  Serial.println("# r     reset to 00");
  Serial.println("# xHH   send exact byte");
  Serial.println("# gHH   goto byte");
  Serial.println("# ?     help");
  Serial.println();

  printCurrent();
}


// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

void loop()
{
  while (Serial.available()) {

    char c = Serial.read();


    // Ignore line endings.
    if (c == '\r' || c == '\n')
      continue;


    // Prevent overflow.
    if (cmdLen >= sizeof(cmd)) {

      cmdLen = 0;

      Serial.println("# ERROR: command buffer overflow");

      continue;
    }


    cmd[cmdLen++] = c;


    // Execute as soon as enough characters have arrived.
    if (commandComplete()) {

      executeCommand();

      cmdLen = 0;
    }
  }
}
