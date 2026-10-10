/*
  Kenwood XS8 USB bridge for Arduino Micro
  Based on xs-tx-test2.ino; preserves its tested XS8 electrical timing.

  Wiring: D2 -- 2.2k -- DATA; D3 -- 2.2k -- BUSY;
          A0 <- DATA; A5 <- BUSY; common GND.
  Serial USB CDC: 115200 (baud nominal on native USB).

  One-byte commands (no newline needed):
    U = start/refresh VOL UP, D = start/refresh VOL DOWN
    H = heartbeat for active volume, S = STOP, M = MUTE, ? = status

  XS8 sequences:
    U: 17 F9 ... 09 07
    D: 17 E9 ... 09 07
    M: 17 D8 07
  Watchdog: 1200ms since last U/D/H. On expiry send 09 then 07.

  IMPORTANT: watchdog cannot guarantee STOP if XS8 bus is permanently busy.
  Pi should also send S on release/shutdown and use its own watchdog.
*/
#include <Arduino.h>

const uint8_t DATA_TX = 2, BUSY_TX = 3;
const uint8_t DATA_RX = A0, BUSY_RX = A5;
const int RX_THRESHOLD = 500;
const uint16_t LEAD_US = 1150, START_US = 9800;
const uint16_t ONE_US = 4900, ZERO_US = 9800, HIGH_US = 4900;
const uint32_t BUS_FREE_TIMEOUT_US = 100000;
const uint16_t BUS_FREE_GUARD_US = 500, END_GUARD_US = 750;
const uint32_t WATCHDOG_MS = 1200;

// 0=idle, 1=up, 2=down. Invariant: context may still need cleanup on TX failure.
uint8_t active = 0;
bool contextOpen = false;
bool stopPending = false;
uint32_t lastHeartbeat = 0;

void releaseLine(uint8_t pin) { pinMode(pin, INPUT); }
void driveHigh(uint8_t pin) { digitalWrite(pin, HIGH); pinMode(pin, OUTPUT); }
void releaseBus() { releaseLine(DATA_TX); releaseLine(BUSY_TX); }
bool rxBusy() { return analogRead(BUSY_RX) > RX_THRESHOLD; }

bool waitBusFree() {
  uint32_t start = micros();
  while ((uint32_t)(micros() - start) < BUS_FREE_TIMEOUT_US) {
    if (rxBusy()) continue;
    uint32_t freeStart = micros();
    while (!rxBusy()) {
      if ((uint32_t)(micros() - freeStart) >= BUS_FREE_GUARD_US) return true;
      if ((uint32_t)(micros() - start) >= BUS_FREE_TIMEOUT_US) return false;
    }
  }
  return false;
}

bool sendXS(uint8_t value) {
  releaseBus();
  if (!waitBusFree()) return false;
  driveHigh(BUSY_TX);
  delayMicroseconds(LEAD_US);
  driveHigh(DATA_TX);
  delayMicroseconds(START_US);
  for (uint8_t i = 0; i < 8; ++i) {
    releaseLine(DATA_TX);
    delayMicroseconds((value & (1 << i)) ? ZERO_US : ONE_US);
    driveHigh(DATA_TX);
    delayMicroseconds(HIGH_US);
  }
  releaseLine(DATA_TX);
  delayMicroseconds(END_GUARD_US);
  releaseLine(BUSY_TX);
  return true;
}

// Return true only when STOP and ROOT were both successfully transmitted.
bool finishVolume() {
  if (stopPending || active != 0) {
    if (!sendXS(0x09)) { Serial.println(F("ERR BUSY STOP")); return false; }
    stopPending = false;
    active = 0;
  }
  if (contextOpen) {
    if (!sendXS(0x07)) { Serial.println(F("ERR BUSY ROOT")); return false; }
    contextOpen = false;
  }
  return true;
}

void beginVolume(uint8_t direction) {
  lastHeartbeat = millis();
  if (active == direction && !stopPending) { Serial.println(F("OK HOLD")); return; }
  if (!finishVolume()) return;
  if (!sendXS(0x17)) { Serial.println(F("ERR BUSY ENTER")); return; }
  contextOpen = true;
  if (!sendXS(direction == 1 ? 0xF9 : 0xE9)) {
    Serial.println(F("ERR BUSY VOLUME"));
    // Context entered, but start not confirmed; attempt ROOT on next loop.
    return;
  }
  active = direction;
  stopPending = true;
  Serial.println(direction == 1 ? F("OK UP") : F("OK DOWN"));
}

void mute() {
  if (!finishVolume()) return;
  if (!sendXS(0x17)) { Serial.println(F("ERR BUSY ENTER")); return; }
  contextOpen = true;
  if (!sendXS(0xD8)) { Serial.println(F("ERR BUSY MUTE")); return; }
  if (!sendXS(0x07)) { Serial.println(F("ERR BUSY ROOT")); return; }
  contextOpen = false;
  Serial.println(F("OK MUTE"));
}

void setup() {
  releaseBus();
  pinMode(DATA_RX, INPUT);
  pinMode(BUSY_RX, INPUT);
  Serial.begin(115200);
  Serial.println(F("XS8 BRIDGE 1.0 READY"));
}

void loop() {
  // A failed STOP/ROOT must be retried before accepting another command.
  if ((active != 0 || stopPending) &&
      (uint32_t)(millis() - lastHeartbeat) >= WATCHDOG_MS) {
    if (finishVolume()) Serial.println(F("OK WATCHDOG STOP"));
  }
  // A failed transaction may leave the alternate context open.
  if (active == 0 && contextOpen && !stopPending) {
    if (sendXS(0x07)) {
      contextOpen = false;
      Serial.println(F("OK ROOT RECOVERED"));
    }
  }

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    if (c == '?') {
      Serial.print(F("STATUS active=")); Serial.print(active);
      Serial.print(F(" context=")); Serial.print(contextOpen);
      Serial.print(F(" stopPending=")); Serial.println(stopPending);
    } else if (c == 'H' || c == 'h') {
      if (active != 0) lastHeartbeat = millis();
      Serial.println(F("OK HEARTBEAT"));
    } else if (c == 'U' || c == 'u') beginVolume(1);
    else if (c == 'D' || c == 'd') beginVolume(2);
    else if (c == 'S' || c == 's') {
      if (finishVolume()) Serial.println(F("OK STOP"));
    } else if (c == 'M' || c == 'm') mute();
    else Serial.println(F("ERR UNKNOWN"));
  }
}

