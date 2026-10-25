#pragma once
#include <Arduino.h>
#include <WiFi.h>

#ifdef USE_OLED
#include <Wire.h>
#include <Adafruit_SSD1306.h>

static const uint8_t OLED_SDA = 5, OLED_SCL = 6, OLED_ADDR = 0x3C;
static const int16_t PANEL_X = 28, PANEL_Y = 24, PANEL_W = 72, PANEL_H = 40, LINE_H = 10;
static const uint32_t PAGE_MS = 4000;

static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static bool oledReady = false;

static void oledFlush() {
  oled.ssd1306_command(SSD1306_PAGEADDR);
  oled.ssd1306_command(PANEL_Y / 8);
  oled.ssd1306_command((PANEL_Y + PANEL_H - 1) / 8);
  oled.ssd1306_command(SSD1306_COLUMNADDR);
  oled.ssd1306_command(PANEL_X);
  oled.ssd1306_command(PANEL_X + PANEL_W - 1);
  const uint8_t* fb = oled.getBuffer();
  for (int p = PANEL_Y / 8; p <= (PANEL_Y + PANEL_H - 1) / 8; p++) {
    const uint8_t* row = fb + p * 128 + PANEL_X;
    for (int off = 0; off < PANEL_W; off += 32) {
      Wire.beginTransmission(OLED_ADDR);
      Wire.write((uint8_t)0x40);
      Wire.write(row + off, min(32, PANEL_W - off));
      Wire.endTransmission();
    }
  }
}

static void oledBegin() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(20);
  oledReady = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
}

static void oledLines(const char* a, const char* b = "", const char* c = "", const char* d = "") {
  if (!oledReady) return;
  const char* l[4] = {a, b, c, d};
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  for (int i = 0; i < 4; i++) { oled.setCursor(PANEL_X, PANEL_Y + i * LINE_H); oled.print(l[i]); }
  oledFlush();
}

static void oledTick(uint32_t blocked, uint32_t allowed, uint32_t domains, bool paused) {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (!oledReady || now - last < 1000) return;
  last = now;
  char a[16], b[16], c[16], d[16];
  switch ((now / PAGE_MS) % 3) {
    case 0: {
      uint32_t total = blocked + allowed;
      snprintf(a, sizeof a, paused ? "PAUSED" : "ADBLOCK");
      snprintf(b, sizeof b, "Blk %lu", (unsigned long)blocked);
      snprintf(c, sizeof c, "%u%% blocked", total ? (unsigned)((uint64_t)blocked * 100 / total) : 0);
      snprintf(d, sizeof d, "%luk domains", (unsigned long)(domains / 1000));
      break;
    }
    case 1:
      snprintf(a, sizeof a, "%s", WiFi.localIP().toString().c_str());
      snprintf(b, sizeof b, "c3adblock");
      snprintf(c, sizeof c, "RSSI %d", WiFi.RSSI());
      d[0] = 0;
      break;
    default: {
      uint32_t up = now / 1000;
      snprintf(a, sizeof a, "Temp %.1fC", temperatureRead());
      snprintf(b, sizeof b, "Heap %luK", (unsigned long)(ESP.getFreeHeap() / 1024));
      snprintf(c, sizeof c, "Up %lud%luh", (unsigned long)(up / 86400), (unsigned long)(up % 86400 / 3600));
      d[0] = 0;
    }
  }
  oledLines(a, b, c, d);
}
#else
static inline void oledBegin() {}
static inline void oledLines(const char*, const char* = "", const char* = "", const char* = "") {}
static inline void oledTick(uint32_t, uint32_t, uint32_t, bool) {}
#endif
