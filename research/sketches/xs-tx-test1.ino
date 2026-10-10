/*
  Kenwood XS Mapper v3
  ====================

  Logical XS representation used by this sketch:

    - byte transmitted LSB first
    - short DATA-low pulse (~4.9 ms) = 0
    - long  DATA-low pulse (~9.8 ms) = 1

  This is equivalent to invert + bit-reverse of the
  representation previously used by KenwoodXS Mapper v2.

  Wiring:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY
    A0 <- DATA
    A5 <- BUSY
    GND common

  Serial: 115200

  Commands:
    xHH   send logical byte HH + listen
    gHH   set current logical byte
    n     send current byte + listen, increment
    a     automatic logical 00..FF scan
    cH    scan logical column H:
          e.g. c7 sends 07,17,27,...,F7
    ?     help

  Examples:

    Logical   Old/legacy representation
    ------------------------------------
      17              17       receiver context ENTER
      07              1F       receiver context EXIT

      F9              60       volume up
      E9              68       volume down
      09              6F       volume stop

      F4              D0       tune up
      E4              D8       tune down

      14              D7       digit 1
      24              DB       digit 2
      34              D3       digit 3
      44              DD       digit 4
      54              D5       digit 5
      64              D9       digit 6
      74              D1       digit 7
      84              DE       digit 8 / context-dependent
      94              D6       digit 9
      04              DF       digit 0
*/


// ============================================================================
// Pins
// ============================================================================

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;

const uint8_t DATA_RX = A0;
const uint8_t BUSY_RX = A5;


// ============================================================================
// RX
// ============================================================================

const int RX_THRESHOLD = 500;


// ============================================================================
// XS timing
// ============================================================================

const uint16_t LEAD_US  = 1150;
const uint16_t START_US = 9800;

const uint16_t SHORT_US = 4900;
const uint16_t LONG_US  = 9800;
const uint16_t HIGH_US  = 4900;

const uint16_t END_US   = 750;

const uint32_t BIT_SPLIT_US = 7200;


// ============================================================================
// General timing
// ============================================================================

const uint32_t LISTEN_MS        = 350;
const uint32_t BETWEEN_BYTES_MS = 200;


// ============================================================================
// State
// ============================================================================

uint8_t currentByte = 0x00;

char cmd[8];
uint8_t cmdLen = 0;


// ============================================================================
// Bus control
// ============================================================================

void releaseBus()
{
  pinMode(DATA_TX, INPUT);
  pinMode(BUSY_TX, INPUT);
}


void driveHigh(uint8_t pin)
{
  digitalWrite(pin, HIGH);
  pinMode(pin, OUTPUT);
}


void releaseLine(uint8_t pin)
{
  pinMode(pin, INPUT);
}


// ============================================================================
// RX helpers
// ============================================================================

bool rxData()
{
  return analogRead(DATA_RX) > RX_THRESHOLD;
}


bool rxBusy()
{
  return analogRead(BUSY_RX) > RX_THRESHOLD;
}


bool waitBusy(bool state, uint32_t timeoutUs)
{
  uint32_t start = micros();

  while ((uint32_t)(micros() - start) < timeoutUs) {
    if (rxBusy() == state)
      return true;
  }

  return false;
}


bool waitData(bool state, uint32_t timeoutUs)
{
  uint32_t start = micros();

  while ((uint32_t)(micros() - start) < timeoutUs) {
    if (rxData() == state)
      return true;
  }

  return false;
}


// ============================================================================
// Legacy/logical conversion
//
// Transform is its own inverse:
//
//   logical = reverse8(~legacy)
//   legacy  = reverse8(~logical)
//
// Used ONLY for diagnostics so we can compare with the existing KenwoodXS
// notation. TX/RX internally use logical representation.
// ============================================================================

uint8_t reverse8(uint8_t x)
{
  x = ((x & 0xF0) >> 4) | ((x & 0x0F) << 4);
  x = ((x & 0xCC) >> 2) | ((x & 0x33) << 2);
  x = ((x & 0xAA) >> 1) | ((x & 0x55) << 1);

  return x;
}


uint8_t logicalToLegacy(uint8_t x)
{
  return reverse8((uint8_t)~x);
}


uint8_t legacyToLogical(uint8_t x)
{
  return reverse8((uint8_t)~x);
}


// ============================================================================
// TX
//
// Logical representation:
//
//   LSB first
//   0 = short low
//   1 = long low
// ============================================================================

void sendXS(uint8_t value)
{
  // BUSY high
  driveHigh(BUSY_TX);
  delayMicroseconds(LEAD_US);

  // START pulse
  driveHigh(DATA_TX);
  delayMicroseconds(START_US);

  // Eight logical bits, LSB first.
  for (uint8_t bit = 0; bit < 8; ++bit) {

    releaseLine(DATA_TX);

    if (value & (1 << bit))
      delayMicroseconds(LONG_US);       // logical 1
    else
      delayMicroseconds(SHORT_US);      // logical 0

    driveHigh(DATA_TX);
    delayMicroseconds(HIGH_US);
  }

  // BUSY falls before DATA.
  releaseLine(BUSY_TX);

  delayMicroseconds(END_US);

  releaseLine(DATA_TX);
}


// ============================================================================
// RX
//
// Decode directly into logical representation:
//
//   first received bit -> bit 0
//   short low = 0
//   long low  = 1
// ============================================================================

bool receiveXS(uint8_t &value)
{
  // BUSY is already HIGH when called.

  // Wait for DATA start pulse.
  if (!waitData(true, 5000))
    return false;

  // End of start pulse / beginning of first bit.
  if (!waitData(false, 15000))
    return false;

  value = 0;

  for (uint8_t bit = 0; bit < 8; ++bit) {

    uint32_t lowStart = micros();

    if (!waitData(true, 15000))
      return false;

    uint32_t lowDuration =
      (uint32_t)(micros() - lowStart);

    // New logical convention:
    //
    // short -> 0
    // long  -> 1

    if (lowDuration >= BIT_SPLIT_US)
      value |= (1 << bit);

    if (bit == 7)
      break;

    if (!waitData(false, 10000))
      return false;
  }

  waitBusy(false, 10000);

  return true;
}


// ============================================================================
// Formatting
// ============================================================================

void printHex(uint8_t value)
{
  if (value < 0x10)
    Serial.print('0');

  Serial.print(value, HEX);
}


void printLogicalAndLegacy(uint8_t value)
{
  Serial.print("0x");
  printHex(value);

  Serial.print(" [legacy 0x");
  printHex(logicalToLegacy(value));
  Serial.print("]");
}


// ============================================================================
// Known logical commands
//
// Only things we currently care about while mapping.
// ============================================================================

const char *knownLogical(uint8_t value)
{
  switch (value) {

    case 0x17:
      return "receiver context ENTER";

    case 0x07:
      return "receiver context EXIT";

    case 0xF9:
      return "VOL UP";

    case 0xE9:
      return "VOL DOWN";

    case 0x09:
      return "VOL STOP";

    case 0xF4:
      return "TUNE UP";

    case 0xE4:
      return "TUNE DOWN";

    case 0xB4:
      return "BAND";

    case 0x04:
      return "KEY 0";

    case 0x14:
      return "KEY 1";

    case 0x24:
      return "KEY 2";

    case 0x34:
      return "KEY 3";

    case 0x44:
      return "KEY 4";

    case 0x54:
      return "KEY 5";

    case 0x64:
      return "KEY 6";

    case 0x74:
      return "KEY 7";

    case 0x84:
      return "KEY 8 / CD (context-dependent)";

    case 0x94:
      return "KEY 9";

    case 0x82:
      return "TUNER";

    case 0x83:
      return "PHONO";

    case 0x85:
      return "TAPE1";

    case 0x9C:
      return "CD STOP";

    case 0xBC:
      return "CD PLAY/PAUSE";

    case 0xEC:
      return "CD PREV";

    case 0xFC:
      return "CD NEXT";

    default:
      return NULL;
  }
}


// ============================================================================
// Listen
// ============================================================================

uint8_t listenForResponses(uint32_t listenMs)
{
  uint32_t listenStartMs = millis();
  uint32_t listenStartUs = micros();

  bool previousBusy = rxBusy();

  uint8_t frameCount = 0;

  while ((uint32_t)(millis() - listenStartMs) < listenMs) {

    bool busy = rxBusy();

    // BUSY rising edge
    if (busy && !previousBusy) {

      uint32_t frameStart = micros();

      uint8_t value;

      if (receiveXS(value)) {

        uint32_t offset =
          (uint32_t)(frameStart - listenStartUs);

        Serial.print("    RX +");
        Serial.print(offset / 1000.0, 1);
        Serial.print(" ms: ");

        printLogicalAndLegacy(value);

        const char *name = knownLogical(value);

        if (name != NULL) {
          Serial.print(" [");
          Serial.print(name);
          Serial.print("]");
        }

        Serial.println();

        frameCount++;
      }

      else {
        Serial.println("    RX: <decode error>");
      }

      previousBusy = rxBusy();
      continue;
    }

    previousBusy = busy;
  }

  if (frameCount == 0)
    Serial.println("    RX: --");

  return frameCount;
}


// ============================================================================
// Transaction
// ============================================================================

void transact(uint8_t value)
{
  Serial.println();

  Serial.print("TX ");
  printLogicalAndLegacy(value);

  const char *name = knownLogical(value);

  if (name != NULL) {
    Serial.print(" [");
    Serial.print(name);
    Serial.print("]");
  }

  Serial.println();

  sendXS(value);

  listenForResponses(LISTEN_MS);
}


// ============================================================================
// Automatic scan
// ============================================================================

void autoScan()
{
  Serial.println();
  Serial.println("======================================");
  Serial.println("# LOGICAL AUTO SCAN 00..FF");
  Serial.println("======================================");

  for (uint16_t i = 0; i <= 0xFF; ++i) {

    uint8_t value = (uint8_t)i;

    transact(value);

    delay(BETWEEN_BYTES_MS);
  }

  Serial.println();
  Serial.println("======================================");
  Serial.println("# AUTO SCAN COMPLETE");
  Serial.println("======================================");
}


// ============================================================================
// Column scan
//
// c7 -> 07,17,27,...,F7
// c4 -> 04,14,24,...,F4
// ============================================================================

void columnScan(uint8_t column)
{
  column &= 0x0F;

  Serial.println();
  Serial.println("======================================");
  Serial.print("# LOGICAL COLUMN SCAN x");
  Serial.println(column, HEX);
  Serial.println("======================================");

  for (uint8_t row = 0; row < 16; ++row) {

    uint8_t value =
      (uint8_t)((row << 4) | column);

    transact(value);

    delay(BETWEEN_BYTES_MS);
  }

  Serial.println();
  Serial.println("======================================");
  Serial.println("# COLUMN SCAN COMPLETE");
  Serial.println("======================================");
}


// ============================================================================
// Hex parser
// ============================================================================

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


// ============================================================================
// Help
// ============================================================================

void printHelp()
{
  Serial.println();
  Serial.println("# Commands");
  Serial.println("#");
  Serial.println("# xHH   send logical byte HH + listen");
  Serial.println("# gHH   set current logical byte");
  Serial.println("# n     send current byte + listen, increment");
  Serial.println("# a     logical 00..FF scan");
  Serial.println("# cH    scan logical column H");
  Serial.println("#       example: c7 -> 07,17,...,F7");
  Serial.println("# ?     help");
  Serial.println();
}


// ============================================================================
// Command execution
// ============================================================================

void executeCommand()
{
  if (cmdLen == 0)
    return;


  // --------------------------------------------------------------------------
  // One-character commands
  // --------------------------------------------------------------------------

  if (cmdLen == 1) {

    char c = cmd[0];

    if (c == 'n' || c == 'N') {

      transact(currentByte);

      currentByte++;

      Serial.print("# NEXT = 0x");
      printHex(currentByte);
      Serial.println();

      return;
    }


    if (c == 'a' || c == 'A') {

      autoScan();
      return;
    }


    if (c == '?') {

      printHelp();
      return;
    }
  }


  // --------------------------------------------------------------------------
  // Column scan: cH
  // --------------------------------------------------------------------------

  if (cmdLen == 2 &&
      (cmd[0] == 'c' || cmd[0] == 'C')) {

    int column = hexDigit(cmd[1]);

    if (column < 0) {

      Serial.println("# ERROR: invalid column");
      return;
    }

    columnScan((uint8_t)column);
    return;
  }


  // --------------------------------------------------------------------------
  // xHH / gHH
  // --------------------------------------------------------------------------

  if (cmdLen == 3) {

    uint8_t value;

    if (!parseHexByte(cmd[1], cmd[2], value)) {

      Serial.println("# ERROR: invalid hex");
      return;
    }


    if (cmd[0] == 'x' || cmd[0] == 'X') {

      transact(value);
      return;
    }


    if (cmd[0] == 'g' || cmd[0] == 'G') {

      currentByte = value;

      Serial.print("# NEXT = 0x");
      printHex(currentByte);
      Serial.println();

      return;
    }
  }


  Serial.print("# ERROR: unknown command: ");

  for (uint8_t i = 0; i < cmdLen; ++i)
    Serial.print(cmd[i]);

  Serial.println();
}


// ============================================================================
// Command completeness
// ============================================================================

bool commandComplete()
{
  if (cmdLen == 0)
    return false;

  char first = cmd[0];

  if (first == 'n' || first == 'N' ||
      first == 'a' || first == 'A' ||
      first == '?') {

    return cmdLen >= 1;
  }


  if (first == 'c' || first == 'C') {

    return cmdLen >= 2;
  }


  if (first == 'x' || first == 'X' ||
      first == 'g' || first == 'G') {

    return cmdLen >= 3;
  }


  return false;
}


// ============================================================================
// Setup
// ============================================================================

void setup()
{
  Serial.begin(115200);

  releaseBus();

  pinMode(DATA_RX, INPUT);
  pinMode(BUSY_RX, INPUT);

  Serial.println();
  Serial.println("# Kenwood XS Mapper v3");
  Serial.println("# logical: LSB-first, short=0, long=1");
  Serial.println("#");
  Serial.println("# D2 = DATA TX");
  Serial.println("# D3 = BUSY TX");
  Serial.println("# A0 = DATA RX");
  Serial.println("# A5 = BUSY RX");

  printHelp();

  Serial.print("# NEXT = 0x");
  printHex(currentByte);
  Serial.println();
}


// ============================================================================
// Main
// ============================================================================

void loop()
{
  while (Serial.available()) {

    char c = Serial.read();

    if (c == '\r' || c == '\n')
      continue;

    if (cmdLen >= sizeof(cmd)) {

      cmdLen = 0;

      Serial.println("# ERROR: command overflow");

      continue;
    }

    cmd[cmdLen++] = c;

    if (commandComplete()) {

      executeCommand();

      cmdLen = 0;
    }
  }
}
