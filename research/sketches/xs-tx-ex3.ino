const uint8_t DATA_RX = A0;
const uint8_t BUSY_RX = A5;

const uint8_t DATA_TX = 2;
const uint8_t BUSY_TX = 3;


void releaseBus()
{
  pinMode(DATA_TX, INPUT);
  pinMode(BUSY_TX, INPUT);
}


void printLevels()
{
  delay(10);

  int data = analogRead(DATA_RX);
  int busy = analogRead(BUSY_RX);

  Serial.print("A0 DATA = ");
  Serial.print(data);

  Serial.print("    A5 BUSY = ");
  Serial.println(busy);
}


void setup()
{
  Serial.begin(115200);

  pinMode(DATA_RX, INPUT);
  pinMode(BUSY_RX, INPUT);

  releaseBus();

  Serial.println();
  Serial.println("# XS level test");
  Serial.println("# d = D2/DATA HIGH");
  Serial.println("# b = D3/BUSY HIGH");
  Serial.println("# r = release both");
  Serial.println("# m = measure only");

  printLevels();
}


void loop()
{
  if (!Serial.available())
    return;

  char c = Serial.read();

  if (c == '\r' || c == '\n')
    return;


  switch (c) {

    case 'd':
      releaseBus();

      digitalWrite(DATA_TX, HIGH);
      pinMode(DATA_TX, OUTPUT);

      Serial.println("# D2 DATA HIGH");
      printLevels();
      break;


    case 'b':
      releaseBus();

      digitalWrite(BUSY_TX, HIGH);
      pinMode(BUSY_TX, OUTPUT);

      Serial.println("# D3 BUSY HIGH");
      printLevels();
      break;


    case 'r':
      releaseBus();

      Serial.println("# RELEASED");
      printLevels();
      break;


    case 'm':
      Serial.println("# MEASURE");
      printLevels();
      break;
  }
}
