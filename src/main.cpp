#include <Arduino.h>

const int PIN = 3;  // D1 on XIAO ESP32C3
const int BAT_PIN = 4;  // D2 on XIAO ESP32C3 (must be ADC1: GPIO0-4), 100k/100k divider from BAT+
volatile unsigned long lastPulse = 0, period = 0;

float readBatteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BAT_PIN);
  return (mv / 16.0f) * 2.0f / 1000.0f;  // x2 to undo the divider
}

int batteryPercent(float v) {
  // Rough linear LiPo estimate: 3.3V = 0%, 4.2V = 100%
  return constrain((int)((v - 3.3f) / (4.2f - 3.3f) * 100.0f), 0, 100);
}

void IRAM_ATTR onPulse() {
  unsigned long now = millis();
  if (now - lastPulse > 30) { period = now - lastPulse; lastPulse = now; }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN), onPulse, FALLING);
  analogSetPinAttenuation(BAT_PIN, ADC_11db);  // full range, ~2.1V max at the pin
}

void loop() {
  bool stopped = millis() - lastPulse > 3000 || period == 0;
  float rpm = stopped ? 0.0 : 60000.0 / period;
  float vbat = readBatteryVolts();
  // Detects a USB host (SOF frames), not a dumb wall charger. VBUS isn't wired to a GPIO on the XIAO.
  if (Serial.isPlugged()) {
    // Charger drives BAT+ toward 4.2V, so the reading isn't the true battery level
    Serial.printf("RPM: %.1f  Battery: %.2fV (on USB, charging/no battery)\n", rpm, vbat);
  } else {
    Serial.printf("RPM: %.1f  Battery: %.2fV (%d%%)\n", rpm, vbat, batteryPercent(vbat));
  }
  delay(1000);
}
