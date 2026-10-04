#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "secrets.h"
#include "page.h"

const int PIN = 3;  // D1 on XIAO ESP32C3
const int BAT_PIN = 4;  // D2 on XIAO ESP32C3 (must be ADC1: GPIO0-4), 100k/100k divider from BAT+
volatile unsigned long lastPulse = 0, period = 0;
volatile uint32_t pulses = 0, glitches = 0;

WebServer server(80);

// Updated once a second in loop(), read by the web handlers
float rpm = 0, maxRpm = 0, vbat = 0;
bool onUsb = false;
unsigned long rideMs = 0, lastTick = 0;
uint32_t revsAtReset = 0, tick = 0;

// One RPM sample per second for the graph
const int HIST_LEN = 300;
uint16_t hist[HIST_LEN];
int histHead = 0, histCount = 0;

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
  unsigned long dt = now - lastPulse;
  if (dt < 30) return;  // contact bounce
  if (dt > 3000 || period == 0) {
    // First pulse after a stop: no valid period yet, wait for the next one
    period = dt > 3000 ? 0 : dt;
  } else if (dt < period * 4 / 10) {
    // Way faster than the last revolution (can't realistically speed up 2.5x in one rev): glitch.
    // lastPulse isn't moved, so the next real pulse is still measured from the last real one.
    glitches++;
    return;
  } else {
    period = dt;
  }
  lastPulse = now;
  pulses++;
}

uint32_t rideRevs() { return pulses - revsAtReset; }
float avgRpm() { return rideMs ? rideRevs() * 60000.0f / rideMs : 0; }

void handleData() {
  char json[256];
  snprintf(json, sizeof(json),
           "{\"tick\":%lu,\"rpm\":%.1f,\"avgRpm\":%.1f,\"maxRpm\":%.1f,\"revs\":%lu,\"rideSec\":%lu,"
           "\"vbat\":%.2f,\"batPct\":%d,\"onUsb\":%s,\"rssi\":%d}",
           tick, rpm, avgRpm(), maxRpm, rideRevs(), rideMs / 1000,
           vbat, batteryPercent(vbat), onUsb ? "true" : "false", WiFi.RSSI());
  server.send(200, "application/json", json);
}

void handleHistory() {
  String json;
  json.reserve(32 + histCount * 4);
  json += "{\"tick\":";
  json += tick;
  json += ",\"rpm\":[";
  for (int i = 0; i < histCount; i++) {
    if (i) json += ',';
    json += hist[(histHead - histCount + i + HIST_LEN) % HIST_LEN];
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleReset() {
  rideMs = 0;
  maxRpm = 0;
  revsAtReset = pulses;
  histCount = 0;
  server.send(204);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN), onPulse, FALLING);
  analogSetPinAttenuation(BAT_PIN, ADC_11db);  // full range, ~2.1V max at the pin

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("bike");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  MDNS.begin("bike");  // http://bike.local
  MDNS.addService("http", "tcp", 80);

  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  lastTick = millis();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastTick < 1000) return;
  bool stopped = now - lastPulse > 3000 || period == 0;
  if (!stopped) rideMs += now - lastTick;
  lastTick = now;

  rpm = stopped ? 0.0 : 60000.0 / period;
  if (rpm > maxRpm) maxRpm = rpm;
  hist[histHead] = (uint16_t)(rpm + 0.5f);
  histHead = (histHead + 1) % HIST_LEN;
  if (histCount < HIST_LEN) histCount++;
  tick++;

  vbat = readBatteryVolts();
  // Detects a USB host (SOF frames), not a dumb wall charger. VBUS isn't wired to a GPIO on the XIAO.
  // On USB the charger drives BAT+ toward 4.2V, so the reading isn't the true battery level.
  onUsb = Serial.isPlugged();

  static wl_status_t lastStatus = WL_IDLE_STATUS;
  if (WiFi.status() != lastStatus) {
    lastStatus = WiFi.status();
    if (lastStatus == WL_CONNECTED) Serial.printf("WiFi connected: http://%s (http://bike.local)\n", WiFi.localIP().toString().c_str());
    else Serial.printf("WiFi status: %d\n", lastStatus);
  }

  Serial.printf("RPM: %.1f  Avg: %.1f  Max: %.1f  Revs: %lu  Ride: %lus  Glitches: %lu  RSSI: %d  Battery: %.2fV ",
                rpm, avgRpm(), maxRpm, rideRevs(), rideMs / 1000, glitches, WiFi.RSSI(), vbat);
  if (onUsb) Serial.println("(on USB, charging/no battery)");
  else Serial.printf("(%d%%)\n", batteryPercent(vbat));
}
