const int TX_PIN  = 2;   // D2
const int ADC_PIN = A1;

const float VREF = 3.3;
const int ADC_MAX = 1023;

float readVoltage() {
  int raw = analogRead(ADC_PIN);
  return raw * VREF / ADC_MAX;
}

void setup() {
  Serial.begin(115200);

  // Released: D2 high impedance, 47k pulls node to GND
  pinMode(TX_PIN, INPUT);

  Serial.println("XS TX bench test");
}

void loop() {
  // RELEASE / idle
  pinMode(TX_PIN, INPUT);
  delay(500);

  int lowRaw = analogRead(ADC_PIN);
  float lowV = lowRaw * VREF / ADC_MAX;

  // DRIVE HIGH
  digitalWrite(TX_PIN, HIGH);
  pinMode(TX_PIN, OUTPUT);
  delay(500);

  int highRaw = analogRead(ADC_PIN);
  float highV = highRaw * VREF / ADC_MAX;

  // Release immediately after measurement
  pinMode(TX_PIN, INPUT);

  Serial.print("LOW: ");
  Serial.print(lowRaw);
  Serial.print("  ");
  Serial.print(lowV, 3);
  Serial.print(" V    HIGH: ");
  Serial.print(highRaw);
  Serial.print("  ");
  Serial.print(highV, 3);
  Serial.println(" V");

  delay(1000);
}
