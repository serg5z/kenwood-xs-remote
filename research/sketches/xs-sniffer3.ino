const uint8_t DATA_PIN = A0;
const uint8_t BUSY_PIN = A5;

const int THRESHOLD = 400;

bool dataState;
bool busyState;
uint32_t startUs;

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  dataState = analogRead(DATA_PIN) > THRESHOLD;
  busyState = analogRead(BUSY_PIN) > THRESHOLD;
  startUs = micros();

  Serial.println("us,DATA,BUSY");
  Serial.print(0);
  Serial.print(',');
  Serial.print(dataState);
  Serial.print(',');
  Serial.println(busyState);
}

void loop() {
  int dataAdc = analogRead(DATA_PIN);
  int busyAdc = analogRead(BUSY_PIN);

  bool newData = dataAdc > THRESHOLD;
  bool newBusy = busyAdc > THRESHOLD;

  if (newData != dataState || newBusy != busyState) {
    uint32_t t = micros() - startUs;

    dataState = newData;
    busyState = newBusy;

    Serial.print(t);
    Serial.print(',');
    Serial.print(dataState);
    Serial.print(',');
    Serial.println(busyState);
  }
}
