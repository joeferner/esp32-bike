#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "secrets.h"

const int PIN = 3;  // D1 on XIAO ESP32C3
const int BAT_PIN = 4;  // D2 on XIAO ESP32C3 (must be ADC1: GPIO0-4), 100k/100k divider from BAT+
volatile unsigned long lastPulse = 0, period = 0;

WebServer server(80);

// Updated once a second in loop(), read by the web handlers
float rpm = 0, vbat = 0;
bool onUsb = false;
unsigned long rideMs = 0, lastTick = 0;

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bike</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:16px;text-align:center}
.v{font-size:4em;font-weight:700;margin:0}.l{color:#999;margin:0 0 24px}
button{font-size:1em;padding:8px 16px}
</style></head><body>
<p class="v" id="rpm">-</p><p class="l">RPM</p>
<p class="v" id="time">-</p><p class="l">Time ridden</p>
<p class="v" id="bat">-</p><p class="l" id="batl">Battery</p>
<button onclick="fetch('/reset',{method:'POST'})">Reset time</button>
<script>
function fmt(s){const h=Math.floor(s/3600),m=Math.floor(s/60)%60,x=s%60;
  return (h?h+':':'')+String(m).padStart(h?2:1,'0')+':'+String(x).padStart(2,'0')}
async function tick(){
  try{
    const d=await (await fetch('/data')).json();
    rpm.textContent=d.rpm.toFixed(0);
    time.textContent=fmt(d.rideSec);
    bat.textContent=d.onUsb?d.vbat.toFixed(2)+'V':d.batPct+'%';
    batl.textContent=d.onUsb?'Battery (on USB)':'Battery ('+d.vbat.toFixed(2)+'V)';
  }catch(e){}
  setTimeout(tick,1000);
}
tick();
</script></body></html>)HTML";

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

void handleData() {
  char json[128];
  snprintf(json, sizeof(json), "{\"rpm\":%.1f,\"rideSec\":%lu,\"vbat\":%.2f,\"batPct\":%d,\"onUsb\":%s}",
           rpm, rideMs / 1000, vbat, batteryPercent(vbat), onUsb ? "true" : "false");
  server.send(200, "application/json", json);
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
  server.on("/reset", HTTP_POST, [] { rideMs = 0; server.send(204); });
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

  if (onUsb) {
    Serial.printf("RPM: %.1f  Ride: %lus  Battery: %.2fV (on USB, charging/no battery)\n", rpm, rideMs / 1000, vbat);
  } else {
    Serial.printf("RPM: %.1f  Ride: %lus  Battery: %.2fV (%d%%)\n", rpm, rideMs / 1000, vbat, batteryPercent(vbat));
  }
}
