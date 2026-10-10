/*
  Kenwood XS8 Mapper / TX Test
  ===========================

  Wiring:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY
    A0 <- DATA
    A5 <- BUSY
    GND common

  Serial: 115200

  Commands:
    xHH   send exact logical byte + listen
    gHH   set current byte
    n     send current byte + listen, increment
    a     automatic 00..FF scan
    ?     help

  Logical byte convention:
    LSB first
    logical 0 = short LOW
    logical 1 = long LOW

  TX arbitration:
    - transmitter waits until BUSY has remained LOW for 500 us
    - BUSY stays asserted for the entire DATA frame
    - DATA is released first
    - BUSY is released after a 750 us end guard
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
// TX timing
// ============================================================================

const uint16_t LEAD_US  = 1150;
const uint16_t START_US = 9800;

const uint16_t ONE_US   = 4900;  // physical short LOW
const uint16_t ZERO_US  = 9800;  // physical long LOW

const uint16_t HIGH_US  = 4900;


// ============================================================================
// Bus arbitration timing
// ============================================================================

// Maximum time we'll wait for BUSY to become free.
const uint32_t BUS_FREE_TIMEOUT_US = 100000;

// BUSY must remain LOW continuously this long before TX.
const uint16_t BUS_FREE_GUARD_US = 500;

// After DATA is released, keep BUSY asserted for this long.
const uint16_t END_GUARD_US = 750;


// ============================================================================
// RX timing
// ============================================================================

const uint32_t BIT_SPLIT_US = 7200;


// ============================================================================
// General timing
// ============================================================================

const uint32_t LISTEN_MS = 350;
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

void releaseLine(uint8_t pin)
{
  pinMode(pin, INPUT);
}


void driveHigh(uint8_t pin)
{
  digitalWrite(pin, HIGH);
  pinMode(pin, OUTPUT);
}


void releaseBus()
{
  releaseLine(DATA_TX);
  releaseLine(BUSY_TX);
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


// ============================================================================
// Wait for bus to become stably free
// ============================================================================

bool waitBusFree()
{
  uint32_t timeoutStart = micros();

  while ((uint32_t)(micros() - timeoutStart) <
         BUS_FREE_TIMEOUT_US) {

    // Someone currently owns the bus.
    if (rxBusy())
      continue;

    // BUSY is LOW.
    // Require it to stay LOW continuously for the guard period.
    uint32_t freeStart = micros();

    while (!rxBusy()) {

      if ((uint32_t)(micros() - freeStart) >=
          BUS_FREE_GUARD_US) {

        return true;
      }

      if ((uint32_t)(micros() - timeoutStart) >=
          BUS_FREE_TIMEOUT_US) {

        return false;
      }
    }

    // BUSY went HIGH during the guard.
    // Restart the stable-free measurement.
  }

  return false;
}


// ============================================================================
// TX
// ============================================================================

bool sendXS(uint8_t value)
{
  // Ensure our TX pins are high-Z before examining the bus.
  releaseBus();

  if (!waitBusFree()) {
    Serial.println("# TX ABORTED: BUSY");
    return false;
  }


  // --------------------------------------------------------------------------
  // Claim bus
  // --------------------------------------------------------------------------

  driveHigh(BUSY_TX);

  delayMicroseconds(LEAD_US);


  // --------------------------------------------------------------------------
  // START
  // --------------------------------------------------------------------------

  driveHigh(DATA_TX);

  delayMicroseconds(START_US);


  // --------------------------------------------------------------------------
  // Logical XS8 byte
  //
  // LSB first
  //
  // logical 0 -> short LOW
  // logical 1 -> long LOW
  // --------------------------------------------------------------------------

  for (uint8_t i = 0; i < 8; ++i) {

    // LOW is obtained by releasing our active-HIGH driver.
    releaseLine(DATA_TX);

    if (value & (1 << i)) {

      // logical 1 = long LOW
      delayMicroseconds(ZERO_US);

    } else {

      // logical 0 = short LOW
      delayMicroseconds(ONE_US);
    }


    driveHigh(DATA_TX);

    delayMicroseconds(HIGH_US);
  }


  // --------------------------------------------------------------------------
  // End frame
  //
  // IMPORTANT:
  //
  // Keep BUSY asserted while DATA is released.
  // Only release BUSY after the complete DATA transaction is finished.
  // --------------------------------------------------------------------------

  releaseLine(DATA_TX);

  delayMicroseconds(END_GUARD_US);

  releaseLine(BUSY_TX);


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


// ============================================================================
// Known observed receiver broadcasts
// ============================================================================

const char *knownStatus(uint8_t value)
{
  switch (value) {

    case 0x71:
      return "TV2";

    case 0x73:
      return "PHONO";

    case 0x79:
      return "TAPE1";

    case 0x7A:
      return "TV1";

    case 0x7B:
      return "TUNER";

    case 0x7D:
      return "CD";

    default:
      return NULL;
  }
}


// ============================================================================
// Wait helpers
// ============================================================================

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
// Decode one received XS8 frame
//
// NOTE:
//
// RX retains the physical/MSB-first representation used by our previous
// sniffer. This is intentional: it preserves continuity with captured
// receiver/changer bytes such as DE, D2, D6, 7D, etc.
// ============================================================================

bool receiveXS8(uint8_t &value)
{
  // BUSY is already HIGH when called.

  if (!waitData(true, 5000))
    return false;


  // End of START pulse / beginning first data LOW.
  if (!waitData(false, 15000))
    return false;


  value = 0;


  for (int bit = 7; bit >= 0; --bit) {

    uint32_t lowStart = micros();


    if (!waitData(true, 15000))
      return false;


    uint32_t lowDuration =
      (uint32_t)(micros() - lowStart);


    bool bitValue =
      lowDuration < BIT_SPLIT_US;


    if (bitValue)
      value |= (1 << bit);


    if (bit == 0)
      break;


    if (!waitData(false, 10000))
      return false;
  }


  waitBusy(false, 10000);

  return true;
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


  while ((uint32_t)(millis() - listenStartMs) <
         listenMs) {

    bool busy = rxBusy();


    // BUSY rising edge
    if (busy && !previousBusy) {

      uint32_t frameStart = micros();

      uint8_t value;


      if (receiveXS8(value)) {

        uint32_t offset =
          (uint32_t)(frameStart - listenStartUs);


        Serial.print("    RX +");
        Serial.print(offset / 1000.0, 1);

        Serial.print(" ms: 0x");
        printHex(value);


        const char *name = knownStatus(value);

        if (name != NULL) {

          Serial.print(" [");
          Serial.print(name);
          Serial.print("]");
        }


        Serial.println();

        frameCount++;

      } else {

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
// Normal transaction
// ============================================================================

void transact(uint8_t value)
{
  Serial.println();

  Serial.print("TX x");
  printHex(value);
  Serial.println();


  if (!sendXS(value)) {

    Serial.println("    TX failed");
    return;
  }


  listenForResponses(LISTEN_MS);
}


// ============================================================================
// Automatic scan
// ============================================================================

void autoScan()
{
  Serial.println();

  Serial.println("======================================");
  Serial.println("# AUTO SCAN x00..xFF");
  Serial.println("======================================");


  for (uint16_t i = 0; i <= 0xFF; ++i) {

    uint8_t value = (uint8_t)i;


    Serial.println();

    Serial.print("# candidate x");
    printHex(value);
    Serial.println();


    transact(value);


    delay(BETWEEN_BYTES_MS);
  }


  Serial.println();

  Serial.println("======================================");
  Serial.println("# AUTO SCAN COMPLETE");
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


bool parseHexByte(
  char hiChar,
  char loChar,
  uint8_t &value)
{
  int hi = hexDigit(hiChar);
  int lo = hexDigit(loChar);


  if (hi < 0 || lo < 0)
    return false;


  value =
    (uint8_t)((hi << 4) | lo);


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
  Serial.println("# n     send current byte + listen");
  Serial.println("# xHH   send exact logical byte + listen");
  Serial.println("# gHH   set current byte");
  Serial.println("# a     automatic x00..xFF scan");
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


      Serial.print("# NEXT = x");
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
  // Three-character commands
  // --------------------------------------------------------------------------

  if (cmdLen == 3) {

    uint8_t value;


    if (!parseHexByte(
          cmd[1],
          cmd[2],
          value)) {

      Serial.println("# ERROR: invalid hex");

      return;
    }


    if (cmd[0] == 'x' ||
        cmd[0] == 'X') {

      transact(value);

      return;
    }


    if (cmd[0] == 'g' ||
        cmd[0] == 'G') {

      currentByte = value;


      Serial.print("# NEXT = x");
      printHex(currentByte);
      Serial.println();

      return;
    }
  }


  // --------------------------------------------------------------------------
  // Unknown command
  // --------------------------------------------------------------------------

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


  if (first == 'n' ||
      first == 'N' ||
      first == 'a' ||
      first == 'A' ||
      first == '?') {

    return cmdLen >= 1;
  }


  if (first == 'x' ||
      first == 'X' ||
      first == 'g' ||
      first == 'G') {

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

  Serial.println("# Kenwood XS8 Mapper / TX Test");
  Serial.println("#");
  Serial.println("# D2 = DATA TX");
  Serial.println("# D3 = BUSY TX");
  Serial.println("# A0 = DATA RX");
  Serial.println("# A5 = BUSY RX");
  Serial.println("#");
  Serial.println("# TX arbitration enabled:");
  Serial.println("# BUSY free guard = 500 us");
  Serial.println("# DATA released before BUSY");


  printHelp();


  Serial.print("# NEXT = x");
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


    if (c == '\r' ||
        c == '\n') {

      continue;
    }


    if (cmdLen >= sizeof(cmd)) {

      cmdLen = 0;

      Serial.println(
        "# ERROR: command overflow");

      continue;
    }


    cmd[cmdLen++] = c;


    if (commandComplete()) {

      executeCommand();

      cmdLen = 0;
    }
  }
}
