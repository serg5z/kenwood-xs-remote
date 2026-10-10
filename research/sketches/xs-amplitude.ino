// Kenwood XS voltage peak measurement
// A1 connected through 10k/10k divider:
// XS line --- 10k ---+--- A1
//                    |
//                   10k
//                    |
//                   GND
//
// ADC reference assumed 3.3 V.
// Divider ratio = 2:1.

const int XS_ADC = A1;

const float ADC_REF = 3.3;
const float DIVIDER = 2.0;

// Change to 4095 if your board uses 12-bit ADC.
// Classic ATmega analogRead() is normally 10-bit (0..1023).
const int ADC_MAX = 1023;

void setup() {
  Serial.begin(115200);
  pinMode(XS_ADC, INPUT);

  Serial.println("Kenwood XS peak catcher");
  Serial.println("Press PLAY/STOP etc.");
}

void loop() {
  static int peak = 0;
  static unsigned long lastReport = 0;

  int raw = analogRead(XS_ADC);

  if (raw > peak) {
    peak = raw;
  }

  // Report peak every second, then start a new window
  if (millis() - lastReport >= 1000) {
    float adcVoltage =
        (peak * ADC_REF) / ADC_MAX;

    float xsVoltage =
        adcVoltage * DIVIDER;

    Serial.print("peak ADC=");
    Serial.print(peak);

    Serial.print("  A1=");
    Serial.print(adcVoltage, 3);

    Serial.print(" V  XS=");
    Serial.print(xsVoltage, 3);

    Serial.println(" V");

    peak = 0;
    lastReport = millis();
  }
}
