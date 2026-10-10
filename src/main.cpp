#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "secrets.h"
#include "page.h"

const int PIN = 3;  // D1 on XIAO ESP32C3 (must be GPIO0-5 to wake from deep sleep)
const int BAT_PIN = 4;  // D2 on XIAO ESP32C3 (must be ADC1: GPIO0-4), 100k/100k divider from BAT+
// Corrects ADC and resistor tolerance: set to (multimeter volts at the battery) / (volts the page shows),
// measured on battery power so it also covers the drop under load. 4.15V metered / 4.08V read.
const float BAT_CAL = 1.017f;
const unsigned long SLEEP_AFTER_MS = 5 * 60 * 1000;  // no pedalling for this long -> deep sleep
// The USB port disappears while asleep: pedal to wake it (or hold BOOT while plugging in) to upload.
const uint32_t WAKE_REVS = 2;               // revs needed after waking to stay awake...
const unsigned long WAKE_WINDOW_MS = 10000;  // ...within this long, else back to sleep
const uint64_t PARKED_POLL_US = 5000000;     // asleep with the magnet on the sensor: check this often
volatile unsigned long lastPulse = 0, period = 0;

WebServer server(80);

// Updated once a second in loop(), read by the web handlers
float rpm = 0, vbat = 0;
bool onUsb = false;
unsigned long lastTick = 0;

// Ride stats live in RTC memory so they survive deep sleep (cleared on power-up or reset)
RTC_DATA_ATTR volatile uint32_t pulses = 0, glitches = 0;
RTC_DATA_ATTR float maxRpm = 0;
RTC_DATA_ATTR unsigned long rideMs = 0;
RTC_DATA_ATTR uint32_t revsAtReset = 0, tick = 0;

// One RPM sample per second for the graph
const int HIST_LEN = 300;
RTC_DATA_ATTR uint16_t hist[HIST_LEN];
RTC_DATA_ATTR int histHead = 0, histCount = 0;

// Diagnostics survive crashes, watchdog and brownout resets (not power loss) so a drop can be
// investigated afterwards via /data. NOINIT memory is garbage on power-up, hence the magic.
const uint32_t DIAG_MAGIC = 0xB1CE0001;
RTC_NOINIT_ATTR struct {
  uint32_t magic, boots, disconnects;
  uint16_t lastReason;
} diag;
esp_reset_reason_t resetReason;
volatile bool gotIp = false;
const unsigned long WIFI_RETRY_MS = 15000;  // still disconnected after this long -> restart the connection

const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    diag.disconnects++;
    diag.lastReason = info.wifi_sta_disconnected.reason;
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    gotIp = true;  // mDNS is restarted from loop(), not from the WiFi event task
  }
}

void startMdns() {
  MDNS.end();
  MDNS.begin("bike");  // http://bike.local
  MDNS.addService("http", "tcp", 80);
}

float readBatteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BAT_PIN);
  return (mv / 16.0f) * 2.0f / 1000.0f * BAT_CAL;  // x2 to undo the divider
}

int batteryPercent(float v) {
  // Typical LiPo discharge curve, interpolated. Top is 4.15V, not 4.2V: the charger stops at ~4.2V
  // and the cell settles to ~4.15V once charging ends.
  static const float volts[] = {3.30f, 3.61f, 3.69f, 3.73f, 3.77f, 3.82f, 3.87f, 3.98f, 4.08f, 4.15f};
  static const int pct[]     = {0,     5,     10,    20,    30,    45,    60,    75,    90,    100};
  const int n = sizeof(pct) / sizeof(pct[0]);
  if (v <= volts[0]) return 0;
  for (int i = 1; i < n; i++)
    if (v < volts[i]) return pct[i - 1] + (v - volts[i - 1]) / (volts[i] - volts[i - 1]) * (pct[i] - pct[i - 1]);
  return 100;
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

void goToSleep(bool log = true) {
  if (log) {
    Serial.println("No pedalling, going to deep sleep");
    Serial.flush();
  }
  if (digitalRead(PIN) == HIGH) {
    // IDF puts a pull-up on LOW-wake pins during deep sleep, so the reed switch closing wakes us
    esp_deep_sleep_enable_gpio_wakeup(BIT(PIN), ESP_GPIO_WAKEUP_GPIO_LOW);
  } else {
    // Magnet stopped on the sensor. Waking on HIGH can't work: IDF forces a pull-down on HIGH-wake
    // pins during deep sleep and the reed switch can only pull to ground, so it would never wake.
    // Poll with a timer until the magnet moves off, then sleep waiting for the next LOW.
    gpio_pullup_dis((gpio_num_t)PIN);  // don't burn current through the closed switch meanwhile
    esp_sleep_enable_timer_wakeup(PARKED_POLL_US);
  }
  esp_deep_sleep_start();
}

// After the sensor wakes us, stay awake only if pedalling continues
void confirmWake() {
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    delayMicroseconds(100);  // let the pull-up settle
    goToSleep(false);        // parked-magnet poll: re-arm based on where the magnet is now
  }
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_GPIO) return;
  uint32_t before = pulses;
  pulses++;  // the rev that woke us happened before the interrupt was attached
  lastPulse = millis() - 5000;  // treat the next pulse as the first after a stop
  while (millis() < WAKE_WINDOW_MS) {
    if (pulses - before > WAKE_REVS) return;
    delay(10);
  }
  pulses = before;  // false alarm (bike bumped), don't count it
  goToSleep();
}

uint32_t rideRevs() { return pulses - revsAtReset; }

unsigned long sleepInSec() {
  unsigned long last = lastPulse;  // snapshot before reading the clock, see loop()
  unsigned long idleMs = millis() - last;
  return idleMs < SLEEP_AFTER_MS ? (SLEEP_AFTER_MS - idleMs) / 1000 : 0;
}
float avgRpm() { return rideMs ? rideRevs() * 60000.0f / rideMs : 0; }

void handleData() {
  char json[448];
  snprintf(json, sizeof(json),
           "{\"tick\":%lu,\"rpm\":%.1f,\"avgRpm\":%.1f,\"maxRpm\":%.1f,\"revs\":%lu,\"rideSec\":%lu,"
           "\"vbat\":%.2f,\"batPct\":%d,\"onUsb\":%s,\"rssi\":%d,"
           "\"sleepSec\":%lu,"
           "\"uptimeSec\":%lu,\"resetReason\":\"%s\",\"boots\":%lu,\"disconnects\":%lu,\"lastDiscReason\":%u}",
           tick, rpm, avgRpm(), maxRpm, rideRevs(), rideMs / 1000,
           vbat, batteryPercent(vbat), onUsb ? "true" : "false", WiFi.RSSI(), sleepInSec(),
           millis() / 1000, resetReasonName(resetReason), diag.boots, diag.disconnects, diag.lastReason);
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
  confirmWake();  // before WiFi, so a false wake costs little power

  resetReason = esp_reset_reason();
  if (diag.magic != DIAG_MAGIC) diag = {DIAG_MAGIC, 0, 0, 0};
  diag.boots++;
  Serial.printf("Boot #%lu, reset reason: %s\n", diag.boots, resetReasonName(resetReason));

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("bike");
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Full TX power on the XIAO C3 causes current spikes that can brown out on battery and
  // often makes the connection worse, not better
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  lastTick = millis();
}

void loop() {
  server.handleClient();

  if (gotIp) {
    gotIp = false;
    startMdns();  // mDNS doesn't survive a reconnect on its own
    Serial.printf("WiFi connected: http://%s (http://bike.local)\n", WiFi.localIP().toString().c_str());
  }

  if (millis() - lastTick < 1000) return;
  // Snapshot the ISR's values, then read the clock, so a pulse mid-calculation can't put
  // lastPulse after `now` (the unsigned subtraction would wrap and trigger an instant sleep)
  // or zero period between the check and the divide.
  unsigned long last = lastPulse, per = period;
  unsigned long now = millis();
  bool stopped = now - last > 3000 || per == 0;
  if (!stopped) rideMs += now - lastTick;
  lastTick = now;

  rpm = stopped ? 0.0 : 60000.0 / per;
  if (rpm > maxRpm) maxRpm = rpm;
  hist[histHead] = (uint16_t)(rpm + 0.5f);
  histHead = (histHead + 1) % HIST_LEN;
  if (histCount < HIST_LEN) histCount++;
  tick++;

  vbat = readBatteryVolts();
  // Detects a USB host (SOF frames), not a dumb wall charger. VBUS isn't wired to a GPIO on the XIAO.
  // On USB the charger drives BAT+ toward 4.2V, so the reading isn't the true battery level.
  onUsb = Serial.isPlugged();
  unsigned long idleMs = now - last;
  if (idleMs > SLEEP_AFTER_MS) goToSleep();

  static wl_status_t lastStatus = WL_IDLE_STATUS;
  static unsigned long disconnectedSince = 0;
  wl_status_t status = WiFi.status();
  if (status != lastStatus) {
    lastStatus = status;
    if (status != WL_CONNECTED) Serial.printf("WiFi status: %d (last disconnect reason: %u)\n", status, diag.lastReason);
  }
  // Auto-reconnect sometimes gives up after certain disconnect reasons, so kick it ourselves
  if (status == WL_CONNECTED) {
    disconnectedSince = 0;
  } else if (!disconnectedSince) {
    disconnectedSince = now;
  } else if (now - disconnectedSince > WIFI_RETRY_MS) {
    Serial.println("WiFi still down, restarting connection");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    disconnectedSince = now;
  }

  Serial.printf("RPM: %.1f  Avg: %.1f  Max: %.1f  Revs: %lu  Ride: %lus  Glitches: %lu  RSSI: %d  Battery: %.2fV ",
                rpm, avgRpm(), maxRpm, rideRevs(), rideMs / 1000, glitches, WiFi.RSSI(), vbat);
  if (onUsb) Serial.print("(on USB, charging/no battery)");
  else Serial.printf("(%d%%)", batteryPercent(vbat));
  Serial.printf("  Sleep in: %lus\n", idleMs < SLEEP_AFTER_MS ? (SLEEP_AFTER_MS - idleMs) / 1000 : 0);
}
