/*
  Kenwood XS Raw ADC Logger

  A0 = DATA
  A5 = BUSY

  Nothing is driven.
  D2/D3 are explicitly high-Z.

  Serial: 115200
*/

const uint8_t DATA_RX = A0;
const uint8_t BUSY_RX = A5;

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;

const int THRESHOLD = 500;

int lastDataLogic = -1;
int lastBusyLogic = -1;


// Take several samples and return the maximum.
// Useful because we're interested in actual HIGH level.
int readMax(uint8_t pin)
{
  int maximum = 0;

  for (int i = 0; i < 8; ++i) {
    int v = analogRead(pin);

    if (v > maximum)
      maximum = v;
  }

  return maximum;
}


void setup()
{
  Serial.begin(115200);

  pinMode(DATA_RX, INPUT);
  pinMode(BUSY_RX, INPUT);

  // Absolutely no TX.
  pinMode(DATA_TX, INPUT);
  pinMode(BUSY_TX, INPUT);

  Serial.println();
  Serial.println("# Kenwood XS RAW ADC logger");
  Serial.println("#");
  Serial.println("# columns:");
  Serial.println("# micros, DATA_ADC, BUSY_ADC, DATA_logic, BUSY_logic");
}


void loop()
{
  int dataADC = readMax(DATA_RX);
  int busyADC = readMax(BUSY_RX);

  int dataLogic = dataADC >= THRESHOLD ? 1 : 0;
  int busyLogic = busyADC >= THRESHOLD ? 1 : 0;


  /*
    Print whenever logical state changes.
  */

  if (dataLogic != lastDataLogic ||
      busyLogic != lastBusyLogic) {

    Serial.print(micros());
    Serial.print(",");

    Serial.print(dataADC);
    Serial.print(",");

    Serial.print(busyADC);
    Serial.print(",");

    Serial.print(dataLogic);
    Serial.print(",");

    Serial.println(busyLogic);


    lastDataLogic = dataLogic;
    lastBusyLogic = busyLogic;
  }
}
