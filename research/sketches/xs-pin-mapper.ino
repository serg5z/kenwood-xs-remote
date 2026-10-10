// Kenwood XS physical pin mapper
//
// DO NOT change wiring yet.
//
// Commands:
//   d = drive D2 HIGH for 2 seconds
//   b = drive D3 HIGH for 2 seconds
//
// A0 and A5 are measured continuously.
//
// TX pins are NEVER actively driven LOW.
// Idle = INPUT / high-Z.

const uint8_t TX_D2 = 2;
const uint8_t TX_D3 = 3;

const uint8_t ADC_A0 = A0;
const uint8_t ADC_A5 = A5;

const uint32_t TEST_TIME_MS = 2000;
const uint32_t PRINT_INTERVAL_MS = 100;

uint8_t activePin = 255;

uint32_t testStartMs = 0;
uint32_t lastPrintMs = 0;


// ------------------------------------------------------------

void releaseLine(uint8_t pin) {
  pinMode(pin, INPUT);
}


void driveHigh(uint8_t pin) {
  digitalWrite(pin, HIGH);
  pinMode(pin, OUTPUT);
}


// ------------------------------------------------------------

void releaseAll() {
  releaseLine(TX_D2);
  releaseLine(TX_D3);

  activePin = 255;
}


void startTest(uint8_t pin) {

  releaseAll();

  Serial.print("# Driving D");
  Serial.print(pin);
  Serial.println(" HIGH for 2 seconds");

  driveHigh(pin);

  activePin = pin;
  testStartMs = millis();
}


// ------------------------------------------------------------

void printADC() {

  int a0 = analogRead(ADC_A0);
  int a5 = analogRead(ADC_A5);

  Serial.print("A0=");
  Serial.print(a0);

  Serial.print("  A5=");
  Serial.print(a5);

  if (activePin != 255) {
    Serial.print("   [D");
    Serial.print(activePin);
    Serial.print(" HIGH]");
  } else {
    Serial.print("   [released]");
  }

  Serial.println();
}


// ------------------------------------------------------------

void setup() {

  Serial.begin(115200);

  while (!Serial) {
  }

  // Safe startup: both TX pins high-Z.
  releaseAll();

  delay(100);

  Serial.println();
  Serial.println("KENWOOD XS PIN MAPPER");
  Serial.println();
  Serial.println("d = D2 HIGH for 2 sec");
  Serial.println("b = D3 HIGH for 2 sec");
  Serial.println();
}


// ------------------------------------------------------------

void loop() {

  uint32_t now = millis();


  // Periodic ADC display
  if (now - lastPrintMs >= PRINT_INTERVAL_MS) {

    lastPrintMs = now;

    printADC();
  }


  // End active test
  if (activePin != 255) {

    if (now - testStartMs >= TEST_TIME_MS) {

      Serial.print("# Releasing D");
      Serial.println(activePin);

      releaseAll();
    }
  }


  // Serial commands
  if (Serial.available()) {

    char c = Serial.read();

    if (c == 'd' || c == 'D') {

      startTest(TX_D2);

    } else if (c == 'b' || c == 'B') {

      startTest(TX_D3);
    }
  }
}
