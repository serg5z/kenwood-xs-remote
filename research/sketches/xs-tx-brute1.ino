/*
  Kenwood XS D0-DF Scanner

  Wiring:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY

  Serial @ 115200:

    n = send current byte and advance
    r = reset to D0
    e = send DE directly (control)

  Start receiver on TV1/TV2.

  Electrical model:
    HIGH = OUTPUT HIGH
    LOW  = INPUT / high-Z
*/

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;


// -----------------------------------------------------------------------------
// Timing measured from KR-V6070
// -----------------------------------------------------------------------------

const uint16_t LEAD_US  = 1150;
const uint16_t START_US = 9800;

const uint16_t ONE_US   = 4900;
const uint16_t ZERO_US  = 9800;

const uint16_t HIGH_US  = 4900;
const uint16_t END_US   = 750;


// -----------------------------------------------------------------------------
// Scanner
// -----------------------------------------------------------------------------

uint8_t currentByte = 0xD0;


// -----------------------------------------------------------------------------
// Bus control
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
  // Idle / relinquish bus
  releaseBus();

  delay(10);


  // BUSY / CTRL rises
  setBusy(true);

  delayMicroseconds(LEAD_US);


  // DATA start
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


  // End of frame
  setBusy(false);

  delayMicroseconds(END_US);

  setData(false);

  releaseBus();
}


// -----------------------------------------------------------------------------
// Display
// -----------------------------------------------------------------------------

void printHex(uint8_t value)
{
  if (value < 0x10)
    Serial.print('0');

  Serial.print(value, HEX);
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

  Serial.println();
}


// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);

  releaseBus();

  Serial.println();
  Serial.println("# Kenwood XS D0-DF scanner");
  Serial.println("#");
  Serial.println("# n = send next byte");
  Serial.println("# r = reset to D0");
  Serial.println("# e = send DE control");
  Serial.println("#");
  Serial.println("# Ready at D0");
}


// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

void loop()
{
  if (!Serial.available())
    return;


  char c = Serial.read();


  if (c == '\r' || c == '\n')
    return;


  // ---------------------------------------------------------------------------
  // NEXT
  // ---------------------------------------------------------------------------

  if (c == 'n' || c == 'N') {

    if (currentByte > 0xDF) {

      Serial.println("# D0-DF complete");
      return;
    }


    sendAndReport(currentByte);


    if (currentByte == 0xDF) {

      Serial.println(
        "# ===== END OF D0-DF RANGE ====="
      );

      // Don't wrap silently.
      currentByte = 0xE0;

    } else {

      currentByte++;
    }

    return;
  }


  // ---------------------------------------------------------------------------
  // RESET
  // ---------------------------------------------------------------------------

  if (c == 'r' || c == 'R') {

    currentByte = 0xD0;

    Serial.println();
    Serial.println("# RESET -> D0");

    return;
  }


  // ---------------------------------------------------------------------------
  // DE positive control
  // ---------------------------------------------------------------------------

  if (c == 'e' || c == 'E') {

    sendAndReport(0xDE);

    return;
  }


  // ---------------------------------------------------------------------------
  // Unknown
  // ---------------------------------------------------------------------------

  Serial.print("# Unknown command: ");
  Serial.println(c);
}
