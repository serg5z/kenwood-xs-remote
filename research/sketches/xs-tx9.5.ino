/*
  Kenwood XS Literal Replay
  Exact captured TV2 / TV1 frames

  Commands:
    1 = replay captured TV1
    2 = replay captured TV2

  TX:
    D2 -> 2.2k -> DATA
    D3 -> 2.2k -> BUSY

  Electrical model:
    HIGH = OUTPUT HIGH
    LOW  = INPUT / high-Z
*/

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;


// Each row:
//   DATA, BUSY, duration_us

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


const uint32_t TV1_FRAME[][3] = {

  {0, 1, 1160},
  {1, 1, 9888},

  {0, 1, 9680},
  {1, 1, 4744},

  {0, 1, 4964},
  {1, 1, 4736},

  {0, 1, 4960},
  {1, 1, 4960},

  {0, 1, 4740},
  {1, 1, 4964},

  {0, 1, 4764},
  {1, 1, 4968},

  {0, 1, 9680},
  {1, 1, 4736},

  {0, 1, 4972},
  {1, 1, 4760},

  {0, 1, 9700},
  {1, 1, 4976}
};


const size_t TV2_COUNT =
  sizeof(TV2_FRAME) / sizeof(TV2_FRAME[0]);

const size_t TV1_COUNT =
  sizeof(TV1_FRAME) / sizeof(TV1_FRAME[0]);


// -----------------------------------------------------------------------------
// Line control
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
// Literal replay
// -----------------------------------------------------------------------------

void replayFrame(const uint32_t frame[][3], size_t count)
{
  releaseBus();

  delay(10);

  for (size_t i = 0; i < count; ++i) {

    setData(frame[i][0] != 0);
    setBusy(frame[i][1] != 0);

    delayMicroseconds(frame[i][2]);
  }

  // Final captured state = 0,0
  setBusy(false);
  setData(false);
}


// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);

  releaseBus();

  Serial.println();
  Serial.println("# Kenwood XS exact literal replay");
  Serial.println("#");
  Serial.println("# 1 = TV1 captured frame");
  Serial.println("# 2 = TV2 captured frame");
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

  switch (c) {

    case '1':

      Serial.println("# REPLAY TV1");
      Serial.flush();

      replayFrame(TV1_FRAME, TV1_COUNT);

      Serial.println("# done");
      break;


    case '2':

      Serial.println("# REPLAY TV2");
      Serial.flush();

      replayFrame(TV2_FRAME, TV2_COUNT);

      Serial.println("# done");
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
