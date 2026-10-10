/*
  Kenwood XS Auto Mapper / Protocol Analyzer
  ==========================================

  TX:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY

  RX:
    A0 <- DATA
    A5 <- BUSY

  Common GND.

  Serial: 115200

  Commands:
    xHH   send one byte and listen for responses
    gHH   set current byte
    n     send current byte, listen, increment
    a     automatic scan 00..FF
    ?     help

  Output example:

    TX 0x3E
      RX +2.8 ms: 0x73
    TX 0x3F
      RX: --

  Known receiver status broadcasts:
    71 = TV2
    73 = PHONO
    79 = TAPE1
    7A = TV1
    7B = TUNER
    7D = CD

  Known receiver commands:
    3E = PHONO
    5E = TAPE1
    B5 = POWER ON
    B9 = POWER OFF
    BE = TUNER
    DE = CD
*/


// ============================================================================
// Pins
// ============================================================================

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;

const uint8_t DATA_RX = A0;
const uint8_t BUSY_RX = A5;


// ============================================================================
// RX threshold
// ============================================================================

// Real LOW ~ 0-30 ADC
// Real HIGH ~ 800-900 ADC
// Arduino HIGH ~ 990 ADC
//
// 500 gives us a huge safety margin.

const int RX_THRESHOLD = 500;


// ============================================================================
// TX timing measured on KR-V6070
// ============================================================================

const uint16_t LEAD_US  = 1150;
const uint16_t START_US = 9800;

const uint16_t ONE_US   = 4900;
const uint16_t ZERO_US  = 9800;

const uint16_t HIGH_US  = 4900;
const uint16_t END_US   = 750;


// ============================================================================
// RX timing classification
// ============================================================================

// LOW duration:
//
// ~4.9 ms = 1
// ~9.8 ms = 0
//
// Anything below ~7.2 ms is therefore 1.

const uint32_t BIT_SPLIT_US = 7200;


// ============================================================================
// Listening / scan timing
// ============================================================================

const uint32_t LISTEN_MS = 500;

const uint32_t BETWEEN_BYTES_MS = 500;

// Receiver needs some time after POWER ON.
const uint32_t POWER_RECOVERY_MS = 2500;


// ============================================================================
// Scanner state
// ============================================================================

uint8_t currentByte = 0x00;


// ============================================================================
// Command parser
// ============================================================================

char cmd[8];
uint8_t cmdLen = 0;


// ============================================================================
// Bus helpers
// ============================================================================

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
// TX
// ============================================================================

void sendXS8(uint8_t value)
{
  releaseBus();

  delay(10);


  // BUSY HIGH
  setBusy(true);

  delayMicroseconds(LEAD_US);


  // DATA start HIGH
  setData(true);

  delayMicroseconds(START_US);


  // 8 bits MSB first
  for (int bit = 7; bit >= 0; --bit) {

    setData(false);

    if (value & (1 << bit))
      delayMicroseconds(ONE_US);
    else
      delayMicroseconds(ZERO_US);


    setData(true);

    delayMicroseconds(HIGH_US);
  }


  // End
  setBusy(false);

  delayMicroseconds(END_US);

  setData(false);

  releaseBus();
}


// ============================================================================
// Printing
// ============================================================================

void printHex(uint8_t value)
{
  if (value < 0x10)
    Serial.print('0');

  Serial.print(value, HEX);
}


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
// Wait for signal state
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
// Receive one XS8 frame
// ============================================================================

bool receiveXS8(uint8_t &value)
{
  /*
     Entry condition:
       BUSY has already gone HIGH.

     Expected:

       BUSY HIGH
          |
          +-- ~1.1 ms
          |
       DATA HIGH start
          |
          +-- ~9.8 ms
          |
       bit LOW
       bit HIGH
       ...
  */


  // Wait for DATA start HIGH.
  if (!waitData(true, 5000))
    return false;


  // Wait for first LOW after start pulse.
  if (!waitData(false, 15000))
    return false;


  value = 0;


  for (int bit = 7; bit >= 0; --bit) {

    // We enter each bit while DATA is LOW.

    uint32_t lowStart = micros();


    // Wait for LOW -> HIGH.
    if (!waitData(true, 15000))
      return false;


    uint32_t lowDuration =
      (uint32_t)(micros() - lowStart);


    bool bitValue =
      lowDuration < BIT_SPLIT_US;


    if (bitValue)
      value |= (1 << bit);


    // Last bit doesn't need us to locate another LOW.
    if (bit == 0)
      break;


    // Wait through fixed HIGH interval until next LOW.
    if (!waitData(false, 10000))
      return false;
  }


  // Allow BUSY/DATA to finish naturally.
  waitBusy(false, 10000);

  return true;
}


// ============================================================================
// Listen for all frames in a window
// ============================================================================

uint8_t listenForResponses(uint32_t listenMs)
{
  uint32_t listenStartMs = millis();
  uint32_t listenStartUs = micros();

  bool previousBusy = rxBusy();

  uint8_t frameCount = 0;


  while ((uint32_t)(millis() - listenStartMs) < listenMs) {

    bool busy = rxBusy();


    // Detect BUSY rising edge.
    if (busy && !previousBusy) {

      uint32_t frameStart = micros();

      uint8_t value;


      if (receiveXS8(value)) {

        uint32_t offset =
          (uint32_t)(frameStart - listenStartUs);


        Serial.print("  RX +");

        Serial.print(offset / 1000.0, 1);

        Serial.print(" ms: 0x");

        printHex(value);


        const char *name = knownStatus(value);

        if (name != NULL) {

          Serial.print("  [");
          Serial.print(name);
          Serial.print("]");
        }


        Serial.println();

        frameCount++;
      }

      else {

        Serial.println("  RX: <decode error>");
      }


      // Re-synchronize after frame.
      previousBusy = rxBusy();

      continue;
    }


    previousBusy = busy;
  }


  if (frameCount == 0)
    Serial.println("  RX: --");


  return frameCount;
}


// ============================================================================
// TX + RX transaction
// ============================================================================

void transact(uint8_t value)
{
  Serial.println();

  Serial.print("TX 0x");
  printHex(value);


  switch (value) {

    case 0x3E:
      Serial.print("  [PHONO cmd]");
      break;

    case 0x5E:
      Serial.print("  [TAPE1 cmd]");
      break;

    case 0xB5:
      Serial.print("  [POWER ON]");
      break;

    case 0xB9:
      Serial.print("  [POWER OFF]");
      break;

    case 0xBE:
      Serial.print("  [TUNER cmd]");
      break;

    case 0xDE:
      Serial.print("  [CD cmd]");
      break;
  }


  Serial.println();


  // Send command.
  sendXS8(value);


  // D2/D3 are now INPUT.
  //
  // Start listening immediately.
  listenForResponses(LISTEN_MS);
}


// ============================================================================
// Automatic scan
// ============================================================================

void autoScan()
{
  Serial.println();
  Serial.println("======================================");
  Serial.println("# AUTO SCAN 00..FF");
  Serial.println("======================================");


  for (uint16_t i = 0; i <= 0xFF; i++) {

    uint8_t value = (uint8_t)i;


    transact(value);


    // ------------------------------------------------------------------------
    // Special handling: B9 powers receiver OFF.
    //
    // Give it a moment, then restore using our confirmed B5.
    // ------------------------------------------------------------------------

    if (value == 0xB9) {

      Serial.println();
      Serial.println("# B9 sent -- restoring receiver with B5");

      delay(1000);


      transact(0xB5);


      Serial.println("# waiting for receiver startup...");

      delay(POWER_RECOVERY_MS);
    }


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
// Execute command
// ============================================================================

void executeCommand()
{
  if (cmdLen == 0)
    return;


  // --------------------------------------------------------------------------
  // Single-character commands
  // --------------------------------------------------------------------------

  if (cmdLen == 1) {

    char c = cmd[0];


    // NEXT
    if (c == 'n' || c == 'N') {

      transact(currentByte);

      currentByte++;

      Serial.print("# NEXT = 0x");
      printHex(currentByte);
      Serial.println();

      return;
    }


    // AUTO
    if (c == 'a' || c == 'A') {

      autoScan();

      return;
    }


    // HELP
    if (c == '?') {

      Serial.println();
      Serial.println("# Commands:");
      Serial.println("#");
      Serial.println("# n     send current byte + listen");
      Serial.println("# xHH   send exact byte + listen");
      Serial.println("# gHH   set current byte");
      Serial.println("# a     automatic 00..FF scan");
      Serial.println("# ?     help");
      Serial.println();

      return;
    }
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


    // EXACT
    if (cmd[0] == 'x' || cmd[0] == 'X') {

      transact(value);

      return;
    }


    // GOTO
    if (cmd[0] == 'g' || cmd[0] == 'G') {

      currentByte = value;

      Serial.print("# NEXT = 0x");
      printHex(currentByte);
      Serial.println();

      return;
    }
  }


  Serial.print("# ERROR: unknown command: ");

  for (uint8_t i = 0; i < cmdLen; i++)
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
  Serial.println("# Kenwood XS Auto Mapper");
  Serial.println("#");
  Serial.println("# TX: D2 DATA, D3 BUSY");
  Serial.println("# RX: A0 DATA, A5 BUSY");
  Serial.println("#");
  Serial.println("# n     next");
  Serial.println("# xHH   exact byte");
  Serial.println("# gHH   goto");
  Serial.println("# a     AUTO 00..FF");
  Serial.println("# ?     help");
  Serial.println();

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


    // Ignore CR/LF.
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
