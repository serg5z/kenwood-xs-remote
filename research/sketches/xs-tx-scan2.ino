/*
  Kenwood XS State-Aware Mapper v2
  ================================

  Wiring:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY
    A0 <- DATA
    A5 <- BUSY
    GND common

  Serial: 115200

  Commands:
    xHH   send exact byte + listen
    gHH   set current byte
    n     send current byte + listen, increment
    a     ordinary automatic 00..FF scan
    i     state-aware controlled scan
    ?     help

  State-aware scan:

    for each candidate XX:

        TX 57       recover source control
        listen

        TX DE       verify source control works
        expect 7D

        TX XX       candidate
        listen

        TX DE       probe source control
        7D => source control still works
        -- => source control may be blocked

        TX 57       recover
        listen

        TX DE       verify recovery
        expect 7D

  Observed behavior:

    11 = source/input control lock ON
         - front panel source selection blocked
         - 3E / 5E / BE / DE blocked
         - POWER still works

    57 = releases source/input lock
         RX 1D, then RX 18

    37 = RX 1D, then RX 18
         but does NOT release source/input lock

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
    9E = TAPE1 (?) second command seen in sweep
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
// RX
// ============================================================================

const int RX_THRESHOLD = 500;


// ============================================================================
// TX timing
// ============================================================================

const uint16_t LEAD_US  = 1150;
const uint16_t START_US = 9800;
const uint16_t ONE_US   = 4900;
const uint16_t ZERO_US  = 9800;
const uint16_t HIGH_US  = 4900;
const uint16_t END_US   = 750;


// ============================================================================
// RX timing
// ============================================================================

const uint32_t BIT_SPLIT_US = 7200;


// ============================================================================
// Timing
// ============================================================================

const uint32_t LISTEN_MS = 350;

// Let state settle between controlled transactions.
const uint32_t STEP_PAUSE_MS = 100;

// Pause between candidates.
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

  setBusy(true);

  delayMicroseconds(LEAD_US);

  setData(true);

  delayMicroseconds(START_US);

  // MSB first
  for (int bit = 7; bit >= 0; --bit) {

    setData(false);

    if (value & (1 << bit))
      delayMicroseconds(ONE_US);
    else
      delayMicroseconds(ZERO_US);

    setData(true);

    delayMicroseconds(HIGH_US);
  }

  setBusy(false);

  delayMicroseconds(END_US);

  setData(false);

  releaseBus();
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
// Decode one frame
// ============================================================================

bool receiveXS8(uint8_t &value)
{
  // BUSY is already HIGH when called.

  if (!waitData(true, 5000))
    return false;

  // End of start pulse / beginning first bit.
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
//
// Optional expectedValue lets controlled scan determine whether a particular
// status byte was observed while still printing every received frame.
// ============================================================================

uint8_t listenForResponses(
  uint32_t listenMs,
  int expectedValue = -1,
  bool *expectedSeen = NULL)
{
  uint32_t listenStartMs = millis();
  uint32_t listenStartUs = micros();

  bool previousBusy = rxBusy();

  uint8_t frameCount = 0;

  if (expectedSeen != NULL)
    *expectedSeen = false;

  while ((uint32_t)(millis() - listenStartMs) < listenMs) {

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

        if (expectedSeen != NULL &&
            expectedValue >= 0 &&
            value == (uint8_t)expectedValue) {

          *expectedSeen = true;
        }
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
// Normal transaction
// ============================================================================

void transact(uint8_t value)
{
  Serial.println();

  Serial.print("TX 0x");
  printHex(value);

  switch (value) {

    case 0x11:
      Serial.print(" [SOURCE LOCK observed]");
      break;

    case 0x37:
      Serial.print(" [unknown: RX 1D,18]");
      break;

    case 0x57:
      Serial.print(" [SOURCE RECOVERY observed]");
      break;

    case 0x3E:
      Serial.print(" [PHONO cmd]");
      break;

    case 0x5E:
      Serial.print(" [TAPE1 cmd]");
      break;

    case 0x9E:
      Serial.print(" [TAPE1? cmd]");
      break;

    case 0xB5:
      Serial.print(" [POWER ON]");
      break;

    case 0xB9:
      Serial.print(" [POWER OFF]");
      break;

    case 0xBE:
      Serial.print(" [TUNER cmd]");
      break;

    case 0xDE:
      Serial.print(" [CD cmd]");
      break;
  }

  Serial.println();

  sendXS8(value);

  listenForResponses(LISTEN_MS);
}


// ============================================================================
// Ordinary auto scan
// ============================================================================

void autoScan()
{
  Serial.println();
  Serial.println("======================================");
  Serial.println("# AUTO SCAN 00..FF");
  Serial.println("======================================");

  for (uint16_t i = 0; i <= 0xFF; i++) {

    uint8_t value = (uint8_t)i;

    // Don't power receiver off during automatic scan.
    if (value == 0xB9) {

      Serial.println();
      Serial.println("TX 0xB9 [SKIPPED: POWER OFF]");

      continue;
    }

    transact(value);

    delay(BETWEEN_BYTES_MS);
  }

  Serial.println();
  Serial.println("======================================");
  Serial.println("# AUTO SCAN COMPLETE");
  Serial.println("======================================");
}


// ============================================================================
// Controlled scan helpers
// ============================================================================

bool sendAndExpect(
  uint8_t txValue,
  uint8_t expectedRx)
{
  bool seen = false;

  sendXS8(txValue);

  listenForResponses(
    LISTEN_MS,
    expectedRx,
    &seen);

  return seen;
}


// ============================================================================
// State-aware controlled scan
// ============================================================================

void controlledInputScan()
{
  Serial.println();
  Serial.println("======================================");
  Serial.println("# STATE-AWARE CONTROLLED SCAN 00..FF");
  Serial.println("#");
  Serial.println("# PREP:    57 -> DE, require RX 7D");
  Serial.println("# TEST:    candidate");
  Serial.println("# PROBE:   DE, look for RX 7D");
  Serial.println("# RECOVER: 57 -> DE, require RX 7D");
  Serial.println("======================================");

  for (uint16_t i = 0; i <= 0xFF; i++) {

    uint8_t candidate = (uint8_t)i;

    // ------------------------------------------------------------------------
    // Skip commands that would interfere badly with automatic testing.
    //
    // DE is infrastructure for this experiment.
    // B9 would power the receiver off.
    // B5 is skipped because power-state testing belongs in a separate scan.
    // 57 is infrastructure/recovery.
    // ------------------------------------------------------------------------

    if (candidate == 0x57 ||
        candidate == 0xB5 ||
        candidate == 0xB9 ||
        candidate == 0xDE) {

      Serial.println();
      Serial.print("CANDIDATE 0x");
      printHex(candidate);
      Serial.println(" [SKIPPED]");

      continue;
    }


    Serial.println();
    Serial.println("--------------------------------------");

    Serial.print("CANDIDATE 0x");
    printHex(candidate);
    Serial.println();


    // ------------------------------------------------------------------------
    // PREP 1: release the observed source/input lock.
    // ------------------------------------------------------------------------

    Serial.println("  PREP TX 0x57 [source recovery]");

    sendXS8(0x57);
    listenForResponses(LISTEN_MS);

    delay(STEP_PAUSE_MS);


    // ------------------------------------------------------------------------
    // PREP 2: verify that source control is actually alive.
    // ------------------------------------------------------------------------

    Serial.println("  VERIFY TX 0xDE [expect CD / 7D]");

    bool baselineOK =
      sendAndExpect(0xDE, 0x7D);

    if (!baselineOK) {

      Serial.println(
        "  *** BASELINE FAILED: 7D NOT SEEN; CANDIDATE SKIPPED ***");

      // One more recovery attempt before moving on.
      Serial.println("  RECOVERY RETRY TX 0x57");

      sendXS8(0x57);
      listenForResponses(LISTEN_MS);

      delay(STEP_PAUSE_MS);

      Serial.println("  RECOVERY VERIFY TX 0xDE");

      bool recovered =
        sendAndExpect(0xDE, 0x7D);

      if (recovered)
        Serial.println("  *** RECOVERY OK ***");
      else
        Serial.println("  *** RECOVERY FAILED ***");

      delay(BETWEEN_BYTES_MS);

      continue;
    }

    Serial.println("  *** BASELINE OK ***");

    delay(STEP_PAUSE_MS);


    // ------------------------------------------------------------------------
    // Candidate
    // ------------------------------------------------------------------------

    Serial.print("  TEST TX 0x");
    printHex(candidate);
    Serial.println();

    sendXS8(candidate);

    listenForResponses(LISTEN_MS);

    delay(STEP_PAUSE_MS);


    // ------------------------------------------------------------------------
    // Probe source control after candidate.
    //
    // If DE no longer produces 7D, candidate changed some relevant state.
    // We deliberately call this SOURCE CONTROL BLOCKED rather than assuming
    // every such candidate has exactly the same semantics as 0x11.
    // ------------------------------------------------------------------------

    Serial.println("  PROBE TX 0xDE [expect CD / 7D]");

    bool sourceControlOK =
      sendAndExpect(0xDE, 0x7D);

    if (sourceControlOK) {

      Serial.println("  RESULT: SOURCE CONTROL OK");
    }

    else {

      Serial.println(
        "  RESULT: *** SOURCE CONTROL BLOCKED / STATE CHANGED ***");
    }

    delay(STEP_PAUSE_MS);


    // ------------------------------------------------------------------------
    // Recovery
    // ------------------------------------------------------------------------

    Serial.println("  RECOVER TX 0x57");

    sendXS8(0x57);
    listenForResponses(LISTEN_MS);

    delay(STEP_PAUSE_MS);


    // ------------------------------------------------------------------------
    // Verify recovery
    // ------------------------------------------------------------------------

    Serial.println("  RECOVERY VERIFY TX 0xDE [expect 7D]");

    bool recoveryOK =
      sendAndExpect(0xDE, 0x7D);

    if (recoveryOK) {

      Serial.println("  *** RECOVERY OK ***");
    }

    else {

      Serial.println(
        "  *** RECOVERY FAILED: STOPPING SCAN ***");

      // Don't continue contaminating subsequent candidates.
      return;
    }


    delay(BETWEEN_BYTES_MS);
  }


  Serial.println();
  Serial.println("======================================");
  Serial.println("# STATE-AWARE CONTROLLED SCAN COMPLETE");
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
// Commands
// ============================================================================

void printHelp()
{
  Serial.println();
  Serial.println("# Commands");
  Serial.println("#");
  Serial.println("# n     send current byte + listen");
  Serial.println("# xHH   send exact byte + listen");
  Serial.println("# gHH   set current byte");
  Serial.println("# a     ordinary 00..FF scan");
  Serial.println("# i     STATE-AWARE controlled scan");
  Serial.println("# ?     help");
  Serial.println();
}


void executeCommand()
{
  if (cmdLen == 0)
    return;


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


    if (c == 'i' || c == 'I') {

      controlledInputScan();

      return;
    }


    if (c == '?') {

      printHelp();

      return;
    }
  }


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

  for (uint8_t i = 0; i < cmdLen; i++)
    Serial.print(cmd[i]);

  Serial.println();
}


bool commandComplete()
{
  if (cmdLen == 0)
    return false;

  char first = cmd[0];

  if (first == 'n' || first == 'N' ||
      first == 'a' || first == 'A' ||
      first == 'i' || first == 'I' ||
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
  Serial.println("# Kenwood XS State-Aware Mapper v2");
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
