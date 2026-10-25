#pragma once
#include <stdint.h>
#include <stdio.h>

static inline void fmtCount(char* o, size_t n, uint32_t v) {
  if (v < 1000) snprintf(o, n, "%lu", (unsigned long)v);
  else if (v < 10000) snprintf(o, n, "%lu.%luk", (unsigned long)(v / 1000), (unsigned long)(v % 1000 / 100));
  else if (v < 1000000) snprintf(o, n, "%luk", (unsigned long)(v / 1000));
  else if (v < 10000000) snprintf(o, n, "%lu.%luM", (unsigned long)(v / 1000000), (unsigned long)(v % 1000000 / 100000));
  else snprintf(o, n, "%luM", (unsigned long)(v / 1000000 > 999 ? 999 : v / 1000000));
}

static inline void fmtUptime(char* o, size_t n, uint32_t s) {
  if (s < 3600) snprintf(o, n, "%lum", (unsigned long)(s / 60));
  else if (s < 48UL * 3600) snprintf(o, n, "%luh", (unsigned long)(s / 3600));
  else snprintf(o, n, "%lud", (unsigned long)(s / 86400 > 999 ? 999 : s / 86400));
}

static inline int rssiBars(int rssi) {
  return rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -85 ? 1 : 0;
}

static inline bool ledOn(uint32_t t, bool paused, bool wifi) {
  return paused ? true : !wifi ? (t / 250) % 2 == 0 : (t % 5000) < 60;
}

static const uint32_t SCREEN_MS = 60000;

static inline bool screenAwake(uint32_t now, uint32_t lastWake) {
  return now - lastWake < SCREEN_MS;
}

static inline bool wakeEvent(bool low, bool prevLow, bool paused, bool prevPaused) {
  return (low && !prevLow) || paused != prevPaused;
}

#ifdef USE_OLED
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>

static const uint8_t OLED_SDA = 5, OLED_SCL = 6, OLED_ADDR = 0x3C;
static const int16_t PANEL_X = 28, PANEL_Y = 24, PANEL_W = 72, PANEL_H = 40, LINE_H = 10, TOP = 1, COL2 = 36;
static const uint8_t LED_PIN = 8;
static int btnPin = -1;

static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static bool oledReady = false;
static uint32_t uiLast = 0, uiLastWake = 0;
static bool uiScreenOn = true;

// ponytail: dirty-page flush still blocks I2C ~2 ms per changed page; move to a task if DNS latency shows
static void oledFlush() {
  static uint8_t shadow[PANEL_H / 8][PANEL_W];
  static bool valid = false;
  const uint8_t* fb = oled.getBuffer();
  for (int i = 0; i < PANEL_H / 8; i++) {
    int p = PANEL_Y / 8 + i;
    const uint8_t* row = fb + p * 128 + PANEL_X;
    if (valid && !memcmp(shadow[i], row, PANEL_W)) continue;
    memcpy(shadow[i], row, PANEL_W);
    oled.ssd1306_command(SSD1306_PAGEADDR);
    oled.ssd1306_command(p);
    oled.ssd1306_command(p);
    oled.ssd1306_command(SSD1306_COLUMNADDR);
    oled.ssd1306_command(PANEL_X);
    oled.ssd1306_command(PANEL_X + PANEL_W - 1);
    for (int off = 0; off < PANEL_W; off += 32) {
      Wire.beginTransmission(OLED_ADDR);
      Wire.write((uint8_t)0x40);
      Wire.write(row + off, min(32, PANEL_W - off));
      if (Wire.endTransmission()) { valid = false; return; }
    }
  }
  valid = true;
}

static void uiBegin() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(20);
  oledReady = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  pinMode(LED_PIN, OUTPUT);
}

static void uiButton(int pin) {
  btnPin = pin;
  pinMode(pin, INPUT_PULLUP);
}

static void oledPut(int x, int line, const char* s) {
  oled.setCursor(PANEL_X + x, PANEL_Y + TOP + line * LINE_H);
  oled.print(s);
}

static void oledStart() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
}

static void uiWake(uint32_t now) {
  uiLastWake = now; uiLast = 0;
  if (!uiScreenOn) { oled.ssd1306_command(SSD1306_DISPLAYON); uiScreenOn = true; }
}

static void oledLines(const char* a, const char* b = "", const char* c = "", const char* d = "") {
  if (!oledReady) return;
  uiWake(millis());
  oledStart();
  oledPut(0, 0, a); oledPut(0, 1, b); oledPut(0, 2, c); oledPut(0, 3, d);
  oledFlush();
}

static void uiTick(uint32_t blocked, uint32_t allowed, int clients, bool paused) {
  static bool prevLow = false, prevPaused = false;
  uint32_t now = millis();
  digitalWrite(LED_PIN, ledOn(now, paused, WiFi.status() == WL_CONNECTED) ? LOW : HIGH);
  bool low = btnPin >= 0 && digitalRead(btnPin) == LOW;
  bool wake = wakeEvent(low, prevLow, paused, prevPaused);
  prevLow = low; prevPaused = paused;
  if (!oledReady) return;
  if (wake) uiWake(now);
  if (uiScreenOn && !screenAwake(now, uiLastWake)) { oled.ssd1306_command(SSD1306_DISPLAYOFF); uiScreenOn = false; }
  if (!uiScreenOn || (uiLast && now - uiLast < 1000)) return;
  uiLast = now ? now : 1;
  char v[8], s[16];
  oledStart();
  oledPut(0, 0, paused ? "PAUSED" : "ADBLOCK");
  int bars = rssiBars(WiFi.RSSI());
  for (int b = 0; b < bars; b++)
    oled.drawFastVLine(PANEL_X + 64 + 2 * b, PANEL_Y + TOP + 6 - 2 * b, 2 * b + 1, SSD1306_WHITE);
  fmtCount(v, sizeof v, blocked); snprintf(s, sizeof s, "B:%s", v); oledPut(0, 1, s);
  fmtCount(v, sizeof v, allowed); snprintf(s, sizeof s, "A:%s", v); oledPut(COL2, 1, s);
  snprintf(s, sizeof s, "C:%d", clients); oledPut(0, 2, s);
  snprintf(s, sizeof s, "W:%d", WiFi.RSSI()); oledPut(COL2, 2, s);
  snprintf(s, sizeof s, "T:%.0fC", temperatureRead()); oledPut(0, 3, s);
  fmtUptime(v, sizeof v, now / 1000); snprintf(s, sizeof s, "U:%s", v); oledPut(COL2, 3, s);
  oledFlush();
}
#else
static inline void uiBegin() {}
static inline void uiButton(int) {}
static inline void oledLines(const char*, const char* = "", const char* = "", const char* = "") {}
static inline void uiTick(uint32_t, uint32_t, int, bool) {}
#endif
