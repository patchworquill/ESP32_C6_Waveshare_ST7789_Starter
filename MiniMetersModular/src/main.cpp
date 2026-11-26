#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ElegantOTA.h>
#include <FastLED.h>
#include <math.h>

#include "LGFX_Waveshare_C6_147.hpp"
#include "secrets.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0-dev"
#endif

static const char *kFirmwareVersion = FIRMWARE_VERSION;

static WebServer server(80);
static LGFX tft;

#define RGB_LED_PIN 8
#define NUM_LEDS 1
static CRGB leds[NUM_LEDS];

static const CRGB kIdleColor = CRGB(255, 69, 0);

// Display backlight: default 30%, hard cap 50%
static const uint8_t kDisplayBrightnessPercent = 30;
static const uint8_t kDisplayBrightnessMaxPercent = 50;

// Pulse / Direct-style palette
static uint16_t colBg;
static uint16_t colBgAccent;
static uint16_t colPrimary;
static uint16_t colText;
static uint16_t colMuted;
static uint16_t colLufs;

static const int kScreenW = 320;
static const int kScreenH = 172;
static const int kLufsW = 48;
static const int kVizW = kScreenW - kLufsW;  // 272
static const uint32_t kInfoOverlayMs = 5000;
static const uint32_t kVizFrameMs = 50;  // ~20 Hz

enum VizMode : int {
  MODE_SPECTRUM = 0,
  MODE_STEREOGRAM,
  MODE_SPECTROGRAM,
  MODE_WAVEFORM,
  MODE_OSCILLOSCOPE,
  MODE_COUNT
};

static const char *kModeNames[MODE_COUNT] = {
    "Spectrum",
    "Stereogram",
    "Spectrogram",
    "Waveform",
    "Oscilloscope",
};

static char hostname[32];
static int g_screenIndex = MODE_SPECTRUM;
static bool g_showInfo = false;
static uint32_t g_infoUntilMs = 0;
static bool g_vizDirty = true;
static uint32_t g_animPhase = 0;

// Spectrogram scroll buffer: one column of heights (0..kScreenH-1 intensity proxy)
static const int kSpecCols = kVizW;
static uint8_t g_specCols[kSpecCols];
static int g_specWrite = 0;

static void setLed(const CRGB &color) {
  leds[0] = color;
  FastLED.show();
}

static uint8_t displayBrightnessByte(uint8_t percent) {
  if (percent > kDisplayBrightnessMaxPercent) {
    percent = kDisplayBrightnessMaxPercent;
  }
  return static_cast<uint8_t>((255 * percent) / 100);
}

static void buildHostname() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t suffix = static_cast<uint16_t>(mac & 0xFFFF);
  snprintf(hostname, sizeof(hostname), "mmm-%04x", suffix);
}

static void initPalette() {
  colBg = tft.color565(0, 0, 0);
  colBgAccent = tft.color565(43, 48, 55);
  colPrimary = tft.color565(172, 192, 222);
  colText = tft.color565(255, 255, 255);
  colMuted = tft.color565(86, 96, 111);
  colLufs = tft.color565(255, 255, 255);
}

static void useMonoFont() {
  tft.setFont(&fonts::Font0);
  tft.setTextSize(1);
  tft.setTextWrap(false);
}

static const char *modeName(int index) {
  if (index < 0 || index >= MODE_COUNT) {
    return "?";
  }
  return kModeNames[index];
}

static void setScreen(int index) {
  if (index < 0) {
    index = 0;
  }
  if (index >= MODE_COUNT) {
    index = MODE_COUNT - 1;
  }
  if (g_screenIndex != index) {
    g_screenIndex = index;
    g_vizDirty = true;
  }
}

static void drawModeLabel() {
  useMonoFont();
  tft.setTextColor(colMuted, colBg);
  tft.setCursor(4, 4);
  tft.print(modeName(g_screenIndex));
}

static void drawLufsStrip() {
  const int x0 = kVizW;
  tft.fillRect(x0, 0, kLufsW, kScreenH, colBgAccent);
  tft.drawFastVLine(x0, 0, kScreenH, colMuted);

  // Dummy M / S / I levels (animated)
  const float t = g_animPhase * 0.07f;
  const float levels[3] = {
      0.55f + 0.35f * sinf(t),
      0.40f + 0.30f * sinf(t * 1.3f + 1.0f),
      0.62f + 0.15f * sinf(t * 0.4f + 2.0f),
  };
  const char *labels[3] = {"M", "S", "I"};
  const int barW = 10;
  const int gap = 4;
  const int topPad = 14;
  const int botPad = 6;
  const int barHMax = kScreenH - topPad - botPad;
  const int startX = x0 + 6;

  useMonoFont();
  for (int i = 0; i < 3; ++i) {
    const int bx = startX + i * (barW + gap);
    const int h = static_cast<int>(levels[i] * barHMax);
    const int by = kScreenH - botPad - h;
    tft.fillRect(bx, topPad, barW, barHMax, colBg);
    tft.fillRect(bx, by, barW, h, colLufs);
    tft.setTextColor(colMuted, colBgAccent);
    tft.setCursor(bx + 2, 3);
    tft.print(labels[i]);
  }
}

static uint16_t heatColor(uint8_t v) {
  // Simple blue -> cyan -> green -> yellow -> red
  const float t = v / 255.0f;
  uint8_t r, g, b;
  if (t < 0.25f) {
    const float u = t / 0.25f;
    r = 0;
    g = static_cast<uint8_t>(u * 180);
    b = 255;
  } else if (t < 0.5f) {
    const float u = (t - 0.25f) / 0.25f;
    r = 0;
    g = 180 + static_cast<uint8_t>(u * 75);
    b = 255 - static_cast<uint8_t>(u * 255);
  } else if (t < 0.75f) {
    const float u = (t - 0.5f) / 0.25f;
    r = static_cast<uint8_t>(u * 255);
    g = 255;
    b = 0;
  } else {
    const float u = (t - 0.75f) / 0.25f;
    r = 255;
    g = 255 - static_cast<uint8_t>(u * 255);
    b = 0;
  }
  return tft.color565(r, g, b);
}

static void drawSpectrum() {
  tft.fillRect(0, 0, kVizW, kScreenH, colBg);
  const int bars = 32;
  const int gap = 1;
  const int barW = (kVizW - 8 - (bars - 1) * gap) / bars;
  const float t = g_animPhase * 0.08f;

  for (int i = 0; i < bars; ++i) {
    const float n = i / static_cast<float>(bars - 1);
    float hNorm = 0.25f + 0.55f * fabsf(sinf(t + n * 4.2f)) * (1.0f - 0.35f * n);
    hNorm += 0.12f * fabsf(sinf(t * 1.7f + i * 0.4f));
    if (hNorm > 1.0f) {
      hNorm = 1.0f;
    }
    const int h = static_cast<int>(hNorm * (kScreenH - 20));
    const int x = 4 + i * (barW + gap);
    const int y = kScreenH - 4 - h;
    tft.fillRect(x, y, barW, h, colPrimary);
    // Peak tick
    tft.drawFastHLine(x, y - 2, barW, colText);
  }
  drawModeLabel();
}

static void drawStereogram() {
  tft.fillRect(0, 0, kVizW, kScreenH, colBg);
  const int cx = kVizW / 2;
  const int cy = kScreenH / 2;
  const float t = g_animPhase * 0.05f;
  const float a = 70.0f + 10.0f * sinf(t * 0.3f);
  const float b = 55.0f + 8.0f * cosf(t * 0.4f);
  int prevX = cx;
  int prevY = cy;

  for (int i = 0; i <= 180; ++i) {
    const float u = i / 180.0f * 6.2831853f;
    const float x = cx + a * sinf(u + t);
    const float y = cy + b * sinf(2.0f * u + t * 1.3f);
    const int ix = static_cast<int>(x);
    const int iy = static_cast<int>(y);
    if (i > 0) {
      tft.drawLine(prevX, prevY, ix, iy, colPrimary);
    }
    prevX = ix;
    prevY = iy;
  }
  tft.drawFastHLine(8, cy, kVizW - 16, colMuted);
  tft.drawFastVLine(cx, 8, kScreenH - 16, colMuted);
  drawModeLabel();
}

static void drawSpectrogram() {
  // Push a new dummy column
  const float t = g_animPhase * 0.09f;
  uint8_t v = static_cast<uint8_t>(
      40 + 180 * fabsf(sinf(t)) * (0.5f + 0.5f * fabsf(sinf(t * 2.1f))));
  g_specCols[g_specWrite] = v;
  g_specWrite = (g_specWrite + 1) % kSpecCols;

  for (int x = 0; x < kVizW; ++x) {
    const int idx = (g_specWrite + x) % kSpecCols;
    const uint8_t c = g_specCols[idx];
    const uint16_t color = heatColor(c);
    // Draw vertical gradient band based on intensity
    const int h = 8 + (c * (kScreenH - 8)) / 255;
    const int y0 = kScreenH - h;
    if (y0 > 0) {
      tft.drawFastVLine(x, 0, y0, colBg);
    }
    tft.drawFastVLine(x, y0, h, color);
  }
  drawModeLabel();
}

static void drawWaveform() {
  tft.fillRect(0, 0, kVizW, kScreenH, colBg);
  tft.drawFastHLine(0, kScreenH / 2, kVizW, colMuted);
  const float t = g_animPhase * 0.12f;
  int prevY = kScreenH / 2;

  for (int x = 0; x < kVizW; ++x) {
    const float n = x / static_cast<float>(kVizW);
    float y = 0.5f * sinf(t + n * 18.0f);
    y += 0.25f * sinf(t * 1.7f + n * 37.0f);
    y += 0.12f * sinf(t * 0.5f + n * 7.0f);
    const int iy = kScreenH / 2 + static_cast<int>(y * (kScreenH * 0.42f));
    if (x > 0) {
      tft.drawLine(x - 1, prevY, x, iy, colPrimary);
    }
    prevY = iy;
  }
  drawModeLabel();
}

static void drawOscilloscope() {
  tft.fillRect(0, 0, kVizW, kScreenH, colBg);
  tft.drawFastHLine(0, kScreenH / 2, kVizW, colMuted);
  const float t = g_animPhase * 0.15f;
  const float freq = 2.0f + 0.5f * sinf(t * 0.2f);
  int prevY = kScreenH / 2;

  for (int x = 0; x < kVizW; ++x) {
    const float n = x / static_cast<float>(kVizW);
    const float y = sinf(t + n * freq * 6.2831853f);
    const int iy = kScreenH / 2 + static_cast<int>(y * (kScreenH * 0.38f));
    if (x > 0) {
      tft.drawLine(x - 1, prevY, x, iy, colPrimary);
    }
    prevY = iy;
  }
  drawModeLabel();
}

static void drawVizFrame() {
  switch (g_screenIndex) {
    case MODE_SPECTRUM:
      drawSpectrum();
      break;
    case MODE_STEREOGRAM:
      drawStereogram();
      break;
    case MODE_SPECTROGRAM:
      drawSpectrogram();
      break;
    case MODE_WAVEFORM:
      drawWaveform();
      break;
    case MODE_OSCILLOSCOPE:
    default:
      drawOscilloscope();
      break;
  }
  drawLufsStrip();
  g_vizDirty = false;
}

static void drawInfoOverlay(const char *wifiLine) {
  tft.fillScreen(colBg);
  useMonoFont();

  auto label = [](int x, int y, const char *s) {
    tft.setTextColor(colMuted, colBg);
    tft.setCursor(x, y);
    tft.print(s);
  };
  auto value = [](int x, int y, const char *s) {
    tft.setTextColor(colText, colBg);
    tft.setCursor(x, y);
    tft.print(s);
  };

  tft.setTextColor(colPrimary, colBg);
  tft.setCursor(8, 8);
  tft.print("MiniMeters Modular");

  label(8, 32, "firmware");
  value(8, 44, kFirmwareVersion);

  label(8, 64, "hostname");
  value(8, 76, hostname);

  label(8, 96, "network");
  value(8, 108, wifiLine);

  label(8, 128, "viz");
  value(8, 140, modeName(g_screenIndex));

  tft.setTextColor(colPrimary, colBg);
  tft.setCursor(8, 158);
  tft.printf("http://%s/control", WiFi.localIP().toString().c_str());
}

static void drawBootConnecting(const char *wifiLine) {
  // Lightweight status during Wi-Fi connect (before palette-driven UI loop)
  tft.fillScreen(TFT_BLACK);
  tft.setFont(&fonts::Font0);
  tft.setTextSize(1);
  tft.setTextWrap(false);
  tft.setTextColor(TFT_LIGHTGREY);
  tft.setCursor(8, 40);
  tft.print("connecting...");
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(8, 60);
  tft.print(wifiLine);
}

static void setupDisplay() {
  tft.init();
  tft.setRotation(1);  // USB connector on the right
  tft.setBrightness(displayBrightnessByte(kDisplayBrightnessPercent));
  initPalette();
  memset(g_specCols, 0, sizeof(g_specCols));
  drawBootConnecting("wifi...");
}

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("WiFi connecting to '%s'", WIFI_SSID);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
    setLed(kIdleColor);
    delay(50);
    setLed(CRGB::Black);
    if (millis() - started > 30000) {
      Serial.println("\nWiFi connect timeout — retrying");
      drawBootConnecting("wifi retry...");
      WiFi.disconnect(true);
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
  Serial.println();
  Serial.printf("WiFi OK  ip=%s  rssi=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

static void setupArduinoOta() {
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    const char *type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.printf("ArduinoOTA start (%s)\n", type);
    setLed(kIdleColor);
    g_showInfo = true;
    g_infoUntilMs = millis() + 600000;  // hold until end/error
    drawInfoOverlay("ArduinoOTA...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nArduinoOTA end");
    setLed(kIdleColor);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("ArduinoOTA progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("ArduinoOTA error[%u]\n", error);
    setLed(CRGB(25, 0, 0));
    drawInfoOverlay("OTA error");
  });

  ArduinoOTA.begin();
  Serial.println("ArduinoOTA ready");
}

static String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '&') {
      out += "&amp;";
    } else if (c == '<') {
      out += "&lt;";
    } else if (c == '>') {
      out += "&gt;";
    } else {
      out += c;
    }
  }
  return out;
}

static void sendStatusPage() {
  String ip = WiFi.localIP().toString();
  String body;
  body.reserve(768);
  body += F("<!doctype html><html><head><meta charset=utf-8>");
  body += F("<meta name=viewport content=\"width=device-width,initial-scale=1\">");
  body += F("<title>MiniMeters Modular</title><style>");
  body += F("body{background:#000;color:#fff;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;");
  body += F("margin:2rem;line-height:1.5} a{color:#ACC0DE} .muted{color:#56606F} .v{color:#ACC0DE}");
  body += F("</style></head><body>");
  body += F("<h1>MiniMeters Modular</h1>");
  body += F("<p class=muted>firmware</p><p class=v>");
  body += htmlEscape(kFirmwareVersion);
  body += F("</p><p class=muted>hostname</p><p class=v>");
  body += htmlEscape(hostname);
  body += F("</p><p class=muted>network</p><p class=v>");
  body += htmlEscape(ip);
  body += F("</p><p class=muted>viz</p><p class=v>");
  body += htmlEscape(modeName(g_screenIndex));
  body += F("</p><p><a href=/control>control</a> &nbsp; <a href=/update>ota /update</a></p>");
  body += F("</body></html>");
  server.send(200, "text/html", body);
}

static void sendControlPage() {
  String body;
  body.reserve(1024);
  body += F("<!doctype html><html><head><meta charset=utf-8>");
  body += F("<meta name=viewport content=\"width=device-width,initial-scale=1\">");
  body += F("<title>Control — MiniMeters</title><style>");
  body += F("body{background:#000;color:#fff;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;");
  body += F("margin:2rem;line-height:1.5} a{color:#ACC0DE;text-decoration:none}");
  body += F(".muted{color:#56606F} .btn{display:block;padding:.75rem 1rem;margin:.4rem 0;");
  body += F("border:1px solid #56606F;color:#ACC0DE} .btn.active{background:#2B3037;border-color:#ACC0DE;color:#fff}");
  body += F("</style></head><body>");
  body += F("<h1>Control</h1>");
  body += F("<p class=muted>viz mode</p>");
  for (int i = 0; i < MODE_COUNT; ++i) {
    body += F("<a class=\"btn");
    if (i == g_screenIndex) {
      body += F(" active");
    }
    body += F("\" href=\"/control?mode=");
    body += String(i);
    body += F("\">");
    body += htmlEscape(kModeNames[i]);
    if (i == g_screenIndex) {
      body += F("  ←");
    }
    body += F("</a>");
  }
  body += F("<p style=\"margin-top:1.5rem\"><a href=/>status</a></p>");
  body += F("</body></html>");
  server.send(200, "text/html", body);
}

static void handleControl() {
  if (server.hasArg("mode")) {
    const int mode = server.arg("mode").toInt();
    setScreen(mode);
    server.sendHeader("Location", "/control", true);
    server.send(302, "text/plain", "");
    return;
  }
  sendControlPage();
}

static void setupWebUi() {
  server.on("/", HTTP_GET, []() { sendStatusPage(); });
  server.on("/control", HTTP_GET, []() { handleControl(); });

  ElegantOTA.begin(&server, "ota", OTA_PASSWORD);
  ElegantOTA.onStart([]() {
    Serial.println("ElegantOTA start");
    setLed(kIdleColor);
    g_showInfo = true;
    g_infoUntilMs = millis() + 600000;
    drawInfoOverlay("ElegantOTA...");
  });
  ElegantOTA.onEnd([](bool success) {
    Serial.printf("ElegantOTA end success=%d\n", success);
    setLed(success ? kIdleColor : CRGB(25, 0, 0));
    if (!success) {
      g_showInfo = true;
      g_infoUntilMs = millis() + 5000;
      drawInfoOverlay("OTA failed");
    } else {
      g_showInfo = false;
      g_vizDirty = true;
    }
  });

  server.begin();
  Serial.println("Web UI ready: /  /control  /update");
}

static void printBanner() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" MiniMetersModular");
  Serial.printf(" version : %s\n", kFirmwareVersion);
  Serial.printf(" hostname: %s\n", hostname);
  Serial.printf(" ip      : %s\n", WiFi.localIP().toString().c_str());
  Serial.printf(" viz     : %s\n", modeName(g_screenIndex));
  Serial.println(" display : LovyanGFX / ST7789 172x320");
  Serial.printf(" control : http://%s/control\n", WiFi.localIP().toString().c_str());
  Serial.printf(" ElegantOTA: http://%s/update  user=ota\n",
                WiFi.localIP().toString().c_str());
  Serial.println("========================================");
}

static void rgbHeartbeat() {
  static uint32_t last = 0;
  static bool on = false;
  const uint32_t now = millis();
  if (now - last < 600) {
    return;
  }
  last = now;
  on = !on;
  setLed(on ? kIdleColor : CRGB::Black);
}

static void updateUi() {
  const uint32_t now = millis();

  if (g_showInfo) {
    if (static_cast<int32_t>(now - g_infoUntilMs) >= 0) {
      g_showInfo = false;
      g_vizDirty = true;
    } else {
      return;
    }
  }

  static uint32_t lastFrame = 0;
  if (!g_vizDirty && (now - lastFrame) < kVizFrameMs) {
    return;
  }
  lastFrame = now;
  ++g_animPhase;
  drawVizFrame();
}

void setup() {
  Serial.begin(115200);
  delay(800);

  FastLED.addLeds<WS2812, RGB_LED_PIN, RGB>(leds, NUM_LEDS);
  FastLED.setBrightness(10);
  setLed(CRGB::Black);

  buildHostname();
  Serial.printf("Booting %s (%s)\n", hostname, kFirmwareVersion);

  setupDisplay();
  connectWifi();
  setupArduinoOta();
  setupWebUi();
  printBanner();

  setLed(kIdleColor);
  g_showInfo = true;
  g_infoUntilMs = millis() + kInfoOverlayMs;
  drawInfoOverlay(WiFi.localIP().toString().c_str());
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  ElegantOTA.loop();
  rgbHeartbeat();
  updateUi();
}
