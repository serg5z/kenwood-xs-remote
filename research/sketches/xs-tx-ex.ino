/*
  Kenwood XS experiment
  Exact captured TV2 waveform + actively driven HIGH/LOW

  TX:
    D2 -> 2.2k -> DATA / SDAT
    D3 -> 2.2k -> BUSY / CTRL

  Serial:
    2 = replay exact captured TV2 frame

  During frame:
    HIGH = OUTPUT HIGH
    LOW  = OUTPUT LOW

  After frame:
    both pins = INPUT / high-Z
*/

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;


// -----------------------------------------------------------------------------
// Exact TV2 capture
//
// Original:
//
// 31093056,0,1
// 31094212,1,1
// 31103892,0,1
// 31113576,1,1
// 31118552,0,1
// 31123304,1,1
// 31128280,0,1
// 31133024,1,1
// 31137988,0,1
// 31142744,1,1
// 31147716,0,1
// 31157408,1,1
// 31162364,0,1
// 31172048,1,1
// 31176788,0,1
// 31186468,1,1
// 31191432,0,1
// 31196400,1,1
// 31201144,0,1
// 31201848,0,0
//
// Format:
//   DATA, BUSY, duration until next state
// -----------------------------------------------------------------------------

const uint32_t TV2_FRAME[][3] = {

  {0, 1, 1156},
  {1, 1, 9680},

  {0, 1, 9684},
  {1, 1, 4976},

  {0, 1, 4752},
  {1, 1, 4976},

  {0, 1, 4744},
  {1, 1, 4964},

  {0, 1, 4756},
  {1, 1, 4972},

  {0, 1, 9692},
  {1, 1, 4956},

  {0, 1, 9684},
  {1, 1, 4740},

  {0, 1, 9680},
  {1, 1, 4964},

  {0, 1, 4968},
  {1, 1, 4744},

  {0, 1, 704}
};


const size_t TV2_COUNT =
  sizeof(TV2_FRAME) / sizeof(TV2_FRAME[0]);


// -----------------------------------------------------------------------------
// Bus control
// -----------------------------------------------------------------------------

void releaseBus()
{
  pinMode(DATA_TX, INPUT);
  pinMode(BUSY_TX, INPUT);
}


void beginDrivenBus()
{
  /*
    Establish LOW before changing to OUTPUT.

    This avoids briefly driving HIGH when the pin mode changes.
  */

  digitalWrite(DATA_TX, LOW);
  digitalWrite(BUSY_TX, LOW);

  pinMode(DATA_TX, OUTPUT);
  pinMode(BUSY_TX, OUTPUT);
}


void driveData(uint8_t value)
{
  digitalWrite(DATA_TX, value ? HIGH : LOW);
}


void driveBusy(uint8_t value)
{
  digitalWrite(BUSY_TX, value ? HIGH : LOW);
}


// -----------------------------------------------------------------------------
// Exact replay
// -----------------------------------------------------------------------------

void replayTV2()
{
  /*
    Start completely disconnected from the bus.
  */

  releaseBus();

  delay(10);


  /*
    From this point until the end of the frame,
    BOTH pins remain OUTPUT.
  */

  beginDrivenBus();


  for (size_t i = 0; i < TV2_COUNT; ++i) {

    driveData(TV2_FRAME[i][0]);
    driveBusy(TV2_FRAME[i][1]);

    delayMicroseconds(TV2_FRAME[i][2]);
  }


  /*
    Captured final state:

      DATA = LOW
      BUSY = LOW

    Drive that state explicitly before releasing.
  */

  driveData(LOW);
  driveBusy(LOW);

  delayMicroseconds(50);


  /*
    Return ownership of shared bus to Kenwood.
  */

  releaseBus();
}


// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);

  releaseBus();

  Serial.println();
  Serial.println("# Kenwood XS exact TV2 active-drive test");
  Serial.println("#");
  Serial.println("# D2 -> 2.2k -> DATA");
  Serial.println("# D3 -> 2.2k -> BUSY");
  Serial.println("#");
  Serial.println("# 2 = exact captured TV2 frame");
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


  if (c == '2') {

    Serial.println("# REPLAY TV2 ACTIVE");
    Serial.flush();

    replayTV2();

    Serial.println("# done");
  }


  else if (c == 'j' || c == 'J') {

    Serial.println();
    Serial.println(
      "========== NEXT BUTTON =========="
    );
  }


  else {

    Serial.print("# Unknown command: ");
    Serial.println(c);
  }
}
