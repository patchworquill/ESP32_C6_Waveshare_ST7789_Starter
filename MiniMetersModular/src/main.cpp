#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ElegantOTA.h>
#include <FastLED.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <string.h>

#include "LGFX_Waveshare_C6_147.hpp"
#include "audio_in.h"
#include "mesh.h"
#include "mmm_proto.h"
#include "secrets.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0-dev"
#endif

static const char *kFirmwareVersion = FIRMWARE_VERSION;

static WebServer server(80);
static LGFX tft;
static Preferences prefs;

#ifndef MMM_LED_PIN
#define MMM_LED_PIN 8
#endif
#ifndef MMM_HEADLESS
#define MMM_HEADLESS 0
#endif
#ifndef MMM_DEFAULT_ROLE
#define MMM_DEFAULT_ROLE ROLE_NODE
#endif
#ifndef MMM_PREFERRED_CONDUCTOR
#define MMM_PREFERRED_CONDUCTOR 0
#endif

#define RGB_LED_PIN MMM_LED_PIN
#define NUM_LEDS 1
static CRGB leds[NUM_LEDS];
static bool g_display = false;

// Display backlight stays at a fixed modest level. The web control is the
// onboard RGB LED, which is bright enough to wash out the panel if left at 100%.
static const uint8_t kDisplayBrightnessPercent = 30;
static const uint8_t kDisplayBrightnessMaxPercent = 50;

static uint16_t colBg;
static uint16_t colBgAccent;
static uint16_t colPrimary;
static uint16_t colText;
static uint16_t colMuted;

static const int kScreenW = 320;
static const int kScreenH = 172;
static const int kLufsW = 48;
static const int kVizW = kScreenW - kLufsW;
static const int kPlotTop = 12;
static const uint32_t kInfoOverlayMs = 5000;

// WebServer only accepts clients inside handleClient(). A full-frame SPI blit
// holds this core long enough that browsers time out, and yield() does not
// drain the TCP stack. Keep each draw to a few milliseconds and leave the
// rest of this slice for the network. 50 ms is ~20 fps, which is enough for
// these meters; drop it for smoother motion once the page still feels quick.
static const uint32_t kVizFrameMs = 50;

static const char *kModeNames[MODE_COUNT] = {
    "Spectrum",
    "Stereogram",
    "Spectrogram",
    "Waveform",
    "Oscilloscope",
};

static char hostname[32];
static int g_screenIndex = MODE_SPECTRUM;
static uint8_t g_role = ROLE_NODE;
static uint8_t g_ledR = 255;
static uint8_t g_ledG = 69;
static uint8_t g_ledB = 0;
static uint8_t g_ledBri = 4;
static uint32_t g_rev = 0;
static uint32_t g_claimGuardUntil = 0;

static bool g_showInfo = false;
static uint32_t g_infoUntilMs = 0;
static bool g_vizDirty = true;
static VizFrame g_viz;
static uint32_t g_drawnSeq = 0xffffffff;

static int g_dotY[kSpecBands];
static int g_peakY[kSpecBands];
static bool g_specInit = false;
static int g_specWrite = 0;
static int g_waveY[kWavePoints];
static bool g_waveValid = false;
static int g_xyX[kStereoPoints];
static int g_xyY[kStereoPoints];
static bool g_xyValid = false;
static int g_lufsH[3] = {-1, -1, -1};

static void serviceNet() {
  ArduinoOTA.handle();
  server.handleClient();
  ElegantOTA.loop();
}

static int clampInt(int v, int lo, int hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

static void applyLed() {
  const uint8_t scale = (uint8_t)((g_ledBri * 255) / 100);
  FastLED.setBrightness(scale);
  leds[0] = CRGB(g_ledR, g_ledG, g_ledB);
  FastLED.show();
}

static void setLedRaw(const CRGB &color) {
  leds[0] = color;
  FastLED.show();
}

static uint8_t displayBrightnessByte(uint8_t percent) {
  if (percent > kDisplayBrightnessMaxPercent) {
    percent = kDisplayBrightnessMaxPercent;
  }
  return (uint8_t)((255 * percent) / 100);
}

static void buildHostname() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t suffix = (uint16_t)(mac & 0xFFFF);
  snprintf(hostname, sizeof(hostname), "mmm-%04x", suffix);
}

static void initPalette() {
  colBg = tft.color565(0, 0, 0);
  colBgAccent = tft.color565(43, 48, 55);
  colPrimary = tft.color565(172, 192, 222);
  colText = tft.color565(255, 255, 255);
  colMuted = tft.color565(86, 96, 111);
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

static const char *roleName(uint8_t role) {
  return role == ROLE_CONDUCTOR ? "conductor" : "node";
}

static void publishIdentity(bool announce) {
  meshSetIdentity(hostname, g_role, (uint8_t)g_screenIndex, g_ledR, g_ledG, g_ledB, g_ledBri,
                  g_rev);
  if (announce) {
    meshAnnounce();
  }
}

static void saveSettings() {
  if (!prefs.begin("mmm", false)) {
    Serial.println("NVS open failed");
    return;
  }
  prefs.putUChar("mode", (uint8_t)g_screenIndex);
  prefs.putUChar("led_r", g_ledR);
  prefs.putUChar("led_g", g_ledG);
  prefs.putUChar("led_b", g_ledB);
  prefs.putUChar("led_bri", g_ledBri);
  prefs.putUChar("role", g_role);
  prefs.putUInt("rev", g_rev);
  prefs.end();
}

static void loadSettings() {
  if (!prefs.begin("mmm", true)) {
    return;
  }
  g_screenIndex = prefs.getUChar("mode", MODE_SPECTRUM);
  g_ledR = prefs.getUChar("led_r", 255);
  g_ledG = prefs.getUChar("led_g", 69);
  g_ledB = prefs.getUChar("led_b", 0);
  g_ledBri = prefs.getUChar("led_bri", 4);
  g_role = prefs.getUChar("role", MMM_DEFAULT_ROLE);
  g_rev = prefs.getUInt("rev", 0);
  prefs.end();
  if (g_screenIndex < 0 || g_screenIndex >= MODE_COUNT) {
    g_screenIndex = MODE_SPECTRUM;
  }
  if (g_role != ROLE_CONDUCTOR) {
    g_role = ROLE_NODE;
  }
  if (g_ledBri > 100) {
    g_ledBri = 100;
  }
}

static void setScreen(int index) {
  index = clampInt(index, 0, MODE_COUNT - 1);
  if (g_screenIndex == index) {
    return;
  }
  g_screenIndex = index;
  g_vizDirty = true;
  audioSetVizMode((uint8_t)g_screenIndex);
}

static void showWaiting() {
  memset(&g_viz, 0, sizeof(g_viz));
  g_drawnSeq = 0xffffffff;
  g_vizDirty = true;
}

static void applyLocalMode(int mode) {
  setScreen(mode);
  saveSettings();
  publishIdentity(true);
}

static void applyLedMesh(uint8_t r, uint8_t g, uint8_t b, uint8_t bri, uint32_t rev, bool broadcast) {
  g_ledR = r;
  g_ledG = g;
  g_ledB = b;
  g_ledBri = bri;
  g_rev = rev;
  applyLed();
  saveSettings();
  publishIdentity(broadcast);
  if (broadcast) {
    meshSendConfig();
  }
}

static void onMeshConfig(uint8_t r, uint8_t g, uint8_t b, uint8_t bri, uint32_t rev) {
  if (rev <= g_rev) {
    return;
  }
  Serial.printf("mesh led rev=%u rgb=%u,%u,%u bri=%u\n", rev, r, g, b, bri);
  applyLedMesh(r, g, b, bri, rev, false);
}

static void onMeshMode(uint8_t mode) {
  if (mode >= MODE_COUNT) {
    return;
  }
  Serial.printf("mesh set viz %s\n", modeName(mode));
  applyLocalMode(mode);
}

static void becomeConductor() {
  g_claimGuardUntil = millis() + 3000;
  if (g_role != ROLE_CONDUCTOR) {
    g_role = ROLE_CONDUCTOR;
    audioSetConductor(true);
    saveSettings();
    showWaiting();
  }
  publishIdentity(true);
  meshSendClaim();
}

static void becomeNode() {
  if (g_role != ROLE_CONDUCTOR) {
    return;
  }
  Serial.println("mesh: now a node");
  g_role = ROLE_NODE;
  audioSetConductor(false);
  g_vizDirty = true;
  saveSettings();
  publishIdentity(true);
}

static void onMeshRole(uint8_t role) {
  if (role == ROLE_CONDUCTOR) {
    Serial.println("mesh set role conductor");
    becomeConductor();
  } else {
    Serial.println("mesh set role node");
    becomeNode();
  }
}

static void onMeshClaim() {
  if ((int32_t)(millis() - g_claimGuardUntil) < 0) {
    return;
  }
  becomeNode();
}

static void resolveConductorRole(uint32_t now) {
#if MMM_PREFERRED_CONDUCTOR
  (void)now;
  return;
#else
  if (g_role != ROLE_CONDUCTOR) {
    return;
  }
  if ((int32_t)(now - g_claimGuardUntil) < 0) {
    return;
  }
  if (meshOtherConductorOutranksUs()) {
    Serial.println("mesh: deferring to the other conductor");
    becomeNode();
  }
#endif
}

static void drawModeLabel() {
  tft.fillRect(0, 0, kVizW, kPlotTop, colBg);
  useMonoFont();
  tft.setTextColor(colMuted, colBg);
  tft.setCursor(4, 3);
  tft.print(modeName(g_screenIndex));
  if (!g_viz.live) {
    tft.print("  no audio");
  }
  if (g_role == ROLE_CONDUCTOR) {
    tft.setCursor(kVizW - 10, 3);
    tft.setTextColor(colMuted, colBg);
    tft.print("C");
  }
}

static void drawLufsChrome() {
  const int x0 = kVizW;
  tft.fillRect(x0, 0, kLufsW, kScreenH, colBgAccent);
  tft.drawFastVLine(x0, 0, kScreenH, colMuted);
  useMonoFont();
  tft.setTextColor(colMuted, colBgAccent);
  const char *labels[3] = {"M", "S", "I"};
  const int barW = 10;
  const int gap = 4;
  const int startX = x0 + 6;
  for (int i = 0; i < 3; ++i) {
    const int bx = startX + i * (barW + gap);
    tft.setCursor(bx + 2, 3);
    tft.print(labels[i]);
  }
}

static void drawLufsBars() {
  const uint8_t levels[3] = {g_viz.lufsM, g_viz.lufsS, g_viz.lufsI};
  const int x0 = kVizW;
  const int barW = 10;
  const int gap = 4;
  const int topPad = 14;
  const int botPad = 6;
  const int barHMax = kScreenH - topPad - botPad;
  const int startX = x0 + 6;

  for (int i = 0; i < 3; ++i) {
    const int h = (int)levels[i] * barHMax / 255;
    const int bx = startX + i * (barW + gap);
    const int prev = g_lufsH[i];
    if (prev < 0) {
      tft.fillRect(bx, topPad, barW, barHMax, colBg);
      if (h > 0) {
        tft.fillRect(bx, kScreenH - botPad - h, barW, h, colPrimary);
      }
    } else if (h != prev) {
      if (h > prev) {
        tft.fillRect(bx, kScreenH - botPad - h, barW, h - prev, colPrimary);
      } else {
        tft.fillRect(bx, kScreenH - botPad - prev, barW, prev - h, colBg);
      }
    }
    g_lufsH[i] = h;
  }
}

// Squared so the noise floor stays near black, and the stops stay well under
// full RGB. A solid bright column is what made the old spectrogram bloom.
static uint16_t heatColor(uint8_t v) {
  float t = v / 255.0f;
  t = t * t;
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  if (t < 0.25f) {
    const float u = t / 0.25f;
    b = (uint8_t)(90 * u);
    g = (uint8_t)(20 * u);
  } else if (t < 0.50f) {
    const float u = (t - 0.25f) / 0.25f;
    b = (uint8_t)(90 * (1.0f - u));
    g = (uint8_t)(20 + 110 * u);
  } else if (t < 0.75f) {
    const float u = (t - 0.50f) / 0.25f;
    g = (uint8_t)(130 + 30 * u);
    r = (uint8_t)(150 * u);
  } else {
    const float u = (t - 0.75f) / 0.25f;
    r = (uint8_t)(150 + 30 * u);
    g = (uint8_t)(160 * (1.0f - u));
  }
  return tft.color565(r, g, b);
}

static void drawSpectrum() {
  const int n = kSpecBands;
  const int slot = kVizW / n;
  const int dot = 3;
  const int base = kScreenH - 5;
  const int maxH = base - kPlotTop - 2;
  if (!g_specInit) {
    tft.drawFastHLine(0, kScreenH - 2, kVizW, colMuted);
  }
  for (int i = 0; i < n; ++i) {
    int h = (int)g_viz.spectrum[i] * maxH / 255;
    int y = base - h;
    int x = i * slot + (slot - dot) / 2;
    int peak = y;
    if (g_specInit) {
      peak = g_peakY[i];
      if (y < peak) {
        peak = y;
      } else if (peak < y) {
        peak += 2;
        if (peak > y) {
          peak = y;
        }
      }
      if (g_dotY[i] == y && g_peakY[i] == peak) {
        continue;
      }
      tft.fillRect(x, g_dotY[i], dot, dot, colBg);
      tft.drawFastHLine(x, g_peakY[i], dot, colBg);
    }
    tft.fillRect(x, y, dot, dot, colPrimary);
    if (peak + 2 < y) {
      tft.drawFastHLine(x, peak, dot, colText);
    }
    g_dotY[i] = y;
    g_peakY[i] = peak;
  }
  g_specInit = true;
}

static void drawSpectrogramColumn() {
  if (!g_viz.live) {
    return;
  }
  const int x = g_specWrite;
  if (x < 0 || x >= kVizW) {
    return;
  }
  const int plotH = kScreenH - kPlotTop;
  for (int i = 0; i < kSpectroBins; ++i) {
    const int y0 = (i * plotH) / kSpectroBins;
    const int y1 = ((i + 1) * plotH) / kSpectroBins;
    const int top = kScreenH - y1;
    int h = y1 - y0;
    if (h < 1) {
      h = 1;
    }
    tft.drawFastVLine(x, top, h, heatColor(g_viz.spectro[i]));
  }
  g_specWrite = (g_specWrite + 1) % kVizW;
}

static int plotMid() {
  return kPlotTop + (kScreenH - kPlotTop) / 2;
}

static int waveX(int i) {
  return (i * (kVizW - 1)) / (kWavePoints - 1);
}

static int waveY(int8_t sample) {
  const int mid = plotMid();
  const int amp = (kScreenH - kPlotTop) * 36 / 100;
  return clampInt(mid - ((int)sample * amp) / 127, kPlotTop + 2, kScreenH - 3);
}

static void strokeWave(const int *ys, uint16_t color) {
  for (int i = 1; i < kWavePoints; ++i) {
    tft.drawLine(waveX(i - 1), ys[i - 1], waveX(i), ys[i], color);
  }
}

static void drawWaveform() {
  int ys[kWavePoints];
  for (int i = 0; i < kWavePoints; ++i) {
    ys[i] = waveY(g_viz.wave[i]);
  }
  if (g_waveValid) {
    strokeWave(g_waveY, colBg);
  }
  tft.drawFastHLine(0, plotMid(), kVizW, colMuted);
  strokeWave(ys, colPrimary);
  memcpy(g_waveY, ys, sizeof(ys));
  g_waveValid = true;
}

static void stereoPoint(int i, int *x, int *y) {
  const int cx = kVizW / 2;
  const int cy = plotMid();
  *x = clampInt(cx + ((int)g_viz.xyL[i] * (cx - 10)) / 127, 1, kVizW - 2);
  *y = clampInt(cy - ((int)g_viz.xyR[i] * (cy - kPlotTop - 8)) / 127, kPlotTop + 1, kScreenH - 2);
}

static void strokeXy(const int *xs, const int *ys, uint16_t color) {
  for (int i = 1; i < kStereoPoints; ++i) {
    tft.drawLine(xs[i - 1], ys[i - 1], xs[i], ys[i], color);
  }
}

static void drawStereogram() {
  int xs[kStereoPoints];
  int ys[kStereoPoints];
  for (int i = 0; i < kStereoPoints; ++i) {
    stereoPoint(i, &xs[i], &ys[i]);
  }
  if (g_xyValid) {
    strokeXy(g_xyX, g_xyY, colBg);
  }
  const int cx = kVizW / 2;
  const int cy = plotMid();
  tft.drawFastHLine(8, cy, kVizW - 16, colMuted);
  tft.drawFastVLine(cx, kPlotTop + 2, kScreenH - kPlotTop - 6, colMuted);
  strokeXy(xs, ys, colPrimary);
  memcpy(g_xyX, xs, sizeof(xs));
  memcpy(g_xyY, ys, sizeof(ys));
  g_xyValid = true;
}

static void clearVizArea() {
  const int band = 24;
  for (int y = 0; y < kScreenH; y += band) {
    int h = band;
    if (y + h > kScreenH) {
      h = kScreenH - y;
    }
    tft.fillRect(0, y, kVizW, h, colBg);
    serviceNet();
  }
}

static void drawInfoOverlay(const char *wifiLine) {
  if (!g_display) {
    return;
  }
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

  label(8, 28, "firmware");
  value(8, 40, kFirmwareVersion);

  label(8, 56, "hostname");
  value(8, 68, hostname);

  label(8, 84, "network");
  value(8, 96, wifiLine);

  char vizLine[40];
  snprintf(vizLine, sizeof(vizLine), "%s · %s", modeName(g_screenIndex), roleName(g_role));
  label(8, 112, "viz");
  value(8, 124, vizLine);

  tft.setTextColor(colPrimary, colBg);
  tft.setCursor(8, 148);
  tft.printf("http://%s/control", WiFi.localIP().toString().c_str());
}

static void drawBootConnecting(const char *wifiLine) {
  if (!g_display) {
    return;
  }
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
#if MMM_HEADLESS
  g_display = false;
  Serial.println("display: headless");
#else
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(displayBrightnessByte(kDisplayBrightnessPercent));
  initPalette();
  g_display = true;
  drawBootConnecting("wifi...");
#endif
}

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(hostname);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("WiFi connecting to '%s'", WIFI_SSID);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
    applyLed();
    delay(40);
    setLedRaw(CRGB::Black);
    if (millis() - started > 30000) {
      Serial.println("\nWiFi connect timeout, retrying");
      drawBootConnecting("wifi retry...");
      WiFi.disconnect(true);
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
  WiFi.setSleep(false);
  Serial.println();
  Serial.printf("WiFi OK  ip=%s  rssi=%d  channel=%d\n", WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), WiFi.channel());
  applyLed();
}

static void setupArduinoOta() {
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    const char *type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.printf("ArduinoOTA start (%s)\n", type);
    applyLed();
    g_showInfo = true;
    g_infoUntilMs = millis() + 600000;
    drawInfoOverlay("ArduinoOTA...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nArduinoOTA end");
    applyLed();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("ArduinoOTA progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("ArduinoOTA error[%u]\n", error);
    setLedRaw(CRGB(80, 0, 0));
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

static void sendNoStore() {
  server.sendHeader("Cache-Control", "no-store");
}

static void appendNodeCard(String &body, const char *name, bool self, uint8_t role, uint8_t mode,
                           const char *ip, int rssi, bool showRssi) {
  body += F("<article class=\"card");
  if (self) {
    body += F(" self");
  }
  body += F("\"><header><h2>");
  body += htmlEscape(name);
  if (self) {
    body += F("<span class=badge>this node</span>");
  }
  body += F("</h2><span class=tip tabindex=0>status<span class=tipbox>");
  if (self) {
    body += F("firmware ");
    body += htmlEscape(kFirmwareVersion);
    body += F("<br>");
  }
  body += htmlEscape(name);
  body += F("<br>");
  body += htmlEscape(ip);
  body += F("<br>");
  body += roleName(role);
  body += F(" · ");
  body += htmlEscape(modeName(mode));
  if (self) {
    body += F("<br>");
    body += meshReady() ? "esp-now" : "esp-now offline";
    body += F(" · ");
    body += String(meshPeerCount());
    body += F(" peers");
  } else if (showRssi) {
    body += F("<br>");
    body += String(rssi);
    body += F(" dBm");
  }
  body += F("</span></span></header>");

  body += F("<p class=ip>");
  body += htmlEscape(ip);
  body += F("</p>");

  body += F("<p class=k>role</p><p class=pills>");
  body += F("<a class=\"pill");
  if (role == ROLE_CONDUCTOR) {
    body += F(" active");
  }
  body += F("\" href=\"/control?node=");
  body += htmlEscape(name);
  body += F("&role=conductor\">Conductor</a><a class=\"pill");
  if (role == ROLE_NODE) {
    body += F(" active");
  }
  body += F("\" href=\"/control?node=");
  body += htmlEscape(name);
  body += F("&role=node\">Node</a></p>");

  body += F("<p class=k>viz</p><p class=pills>");
  for (int i = 0; i < MODE_COUNT; ++i) {
    body += F("<a class=\"pill");
    if (i == (int)mode) {
      body += F(" active");
    }
    body += F("\" href=\"/control?node=");
    body += htmlEscape(name);
    body += F("&mode=");
    body += String(i);
    body += F("\">");
    body += htmlEscape(kModeNames[i]);
    body += F("</a>");
  }
  body += F("</p>");

  if (role == ROLE_CONDUCTOR && ip[0]) {
    body += F("<p class=k>stream</p><p class=v>udp://");
    body += htmlEscape(ip);
    body += F(":");
    body += String(kAudioUdpPort);
    body += F("</p><p class=muted>python stream_audio.py ");
    body += htmlEscape(ip);
    body += F("</p>");
  }

  if (ip[0]) {
    body += F("<p class=links><a href=\"http://");
    body += ip;
    body += F("/control\">control</a><a href=\"http://");
    body += ip;
    body += F("/update\">ota /update</a></p>");
  }
  body += F("</article>");
}

static void sendControlPage() {
  String ip = WiFi.localIP().toString();
  char color[8];
  snprintf(color, sizeof(color), "#%02x%02x%02x", g_ledR, g_ledG, g_ledB);

  String body;
  body.reserve(8192);
  body += F("<!doctype html><html><head><meta charset=utf-8>");
  body += F("<meta name=viewport content=\"width=device-width,initial-scale=1\">");
  body += F("<title>MiniMeters Modular</title><style>");
  body += F("body{background:#000;color:#fff;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;");
  body += F("margin:1.5rem;line-height:1.45} a{color:#ACC0DE;text-decoration:none}");
  body += F("h1{font-size:1.25rem;font-weight:500;margin:0 0 .25rem} h2{font-size:1rem;font-weight:500;margin:0}");
  body += F(".muted{color:#56606F} .v{color:#ACC0DE} .k{color:#56606F;margin:.8rem 0 .25rem}");
  body += F(".cards{display:flex;flex-wrap:wrap;gap:1rem;margin-top:1.25rem}");
  body += F(".card{flex:1 1 260px;border:1px solid #56606F;padding:1rem;box-sizing:border-box}");
  body += F(".card.self{border-color:#ACC0DE}");
  body += F("header{display:flex;justify-content:space-between;align-items:flex-start;gap:.75rem}");
  body += F(".badge{margin-left:.5rem;color:#ACC0DE;font-size:11px;border:1px solid #ACC0DE;padding:.1rem .35rem}");
  body += F(".ip{color:#ACC0DE;margin:.4rem 0 0}");
  body += F(".pills{display:flex;flex-wrap:wrap;gap:.35rem;margin:0}");
  body += F(".pill{border:1px solid #56606F;color:#ACC0DE;padding:.3rem .5rem;font-size:12px}");
  body += F(".pill.active{background:#2B3037;border-color:#ACC0DE;color:#fff}");
  body += F(".links{margin:1rem 0 0}.links a{margin-right:1rem}");
  body += F(".tip{position:relative;color:#56606F;font-size:11px;cursor:help;border-bottom:1px dotted #56606F}");
  body += F(".tipbox{display:none;position:absolute;z-index:2;right:0;top:1.3rem;background:#2B3037;color:#fff;");
  body += F("padding:.6rem .75rem;min-width:12rem;font-size:12px;line-height:1.45}");
  body += F(".tip:hover .tipbox,.tip:focus .tipbox{display:block}");
  body += F("input[type=color]{background:#000;border:1px solid #56606F;height:2.2rem;width:3.2rem;padding:0}");
  body += F("input[type=range]{width:16rem;max-width:100%;accent-color:#ACC0DE}");
  body += F("button{font:inherit;background:#000;color:#ACC0DE;border:1px solid #56606F;padding:.45rem .7rem;cursor:pointer}");
  body += F(".led{margin-top:1.5rem;padding-top:1rem;border-top:1px solid #2B3037}");
  body += F(".ver{color:#56606F;font-size:11px;margin-top:2rem}");
  body += F("</style></head><body>");
  body += F("<h1>MiniMeters Modular</h1>");
  body += F("<p class=muted>Each node keeps its own visualizer. LED color and brightness apply to every node.</p>");

  body += F("<div class=cards>");
  appendNodeCard(body, hostname, true, g_role, (uint8_t)g_screenIndex, ip.c_str(), 0, false);
  MeshPeerInfo peers[8];
  const int n = meshCopyPeers(peers, 8);
  for (int i = 0; i < n; ++i) {
    char ipbuf[20];
    snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", peers[i].ip[0], peers[i].ip[1], peers[i].ip[2],
             peers[i].ip[3]);
    const char *peerName = peers[i].name[0] ? peers[i].name : "peer";
    appendNodeCard(body, peerName, false, peers[i].role, peers[i].mode, ipbuf, peers[i].rssi, true);
  }
  body += F("</div>");

  body += F("<form class=led action=/control method=get>");
  body += F("<p class=k>led, all nodes</p>");
  body += F("<p><input type=color name=led value=\"");
  body += color;
  body += F("\"> <span class=muted>brightness <span id=bv>");
  body += String(g_ledBri);
  body += F("</span>%</span></p>");
  body += F("<p><input id=bri type=range name=bri min=0 max=100 value=\"");
  body += String(g_ledBri);
  body += F("\"> <button type=submit>Apply LED</button></p></form>");
  body += F("<script>var b=document.getElementById('bri'),o=document.getElementById('bv');");
  body += F("if(b&&o)b.oninput=function(){o.textContent=b.value}</script>");
  body += F("<p class=ver>firmware ");
  body += htmlEscape(kFirmwareVersion);
  body += F("</p></body></html>");
  sendNoStore();
  server.send(200, "text/html", body);
}

static bool parseColor(const String &in, uint8_t *r, uint8_t *g, uint8_t *b) {
  String hex = in;
  if (hex.startsWith("#")) {
    hex = hex.substring(1);
  }
  if (hex.length() != 6) {
    return false;
  }
  char buf[3] = {0, 0, 0};
  auto byteAt = [&](int index, uint8_t *out) -> bool {
    buf[0] = hex[index];
    buf[1] = hex[index + 1];
    char *end = nullptr;
    const long v = strtol(buf, &end, 16);
    if (end != buf + 2 || v < 0 || v > 255) {
      return false;
    }
    *out = (uint8_t)v;
    return true;
  };
  return byteAt(0, r) && byteAt(2, g) && byteAt(4, b);
}

static void redirectControl() {
  server.sendHeader("Location", "/control", true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

static void handleControl() {
  const bool forPeer = server.hasArg("node") && server.arg("node") != hostname;
  MeshPeerInfo peer;
  const bool peerOk = forPeer && meshFindPeer(server.arg("node").c_str(), &peer);

  if (server.hasArg("mode")) {
    const int next = server.arg("mode").toInt();
    if (next >= 0 && next < MODE_COUNT) {
      if (!forPeer) {
        if (next != g_screenIndex) {
          applyLocalMode(next);
        }
      } else if (peerOk && peer.mode != (uint8_t)next) {
        meshSendSetMode(peer.mac, (uint8_t)next);
      }
    }
  }

  uint8_t r = g_ledR;
  uint8_t g = g_ledG;
  uint8_t b = g_ledB;
  uint8_t bri = g_ledBri;
  bool ledChanged = false;
  if (server.hasArg("led")) {
    uint8_t nr, ng, nb;
    if (parseColor(server.arg("led"), &nr, &ng, &nb) &&
        (nr != g_ledR || ng != g_ledG || nb != g_ledB)) {
      r = nr;
      g = ng;
      b = nb;
      ledChanged = true;
    }
  }
  if (server.hasArg("bri")) {
    int next = clampInt(server.arg("bri").toInt(), 0, 100);
    if ((uint8_t)next != g_ledBri) {
      bri = (uint8_t)next;
      ledChanged = true;
    }
  }
  if (ledChanged) {
    applyLedMesh(r, g, b, bri, g_rev + 1, true);
  }

  if (server.hasArg("role")) {
    const String rs = server.arg("role");
    const uint8_t want = rs == "conductor" ? ROLE_CONDUCTOR : ROLE_NODE;
    if (rs == "conductor" || rs == "node") {
      if (!forPeer) {
        if (want == ROLE_CONDUCTOR) {
          becomeConductor();
        } else if (g_role != ROLE_NODE) {
          becomeNode();
        }
      } else if (peerOk && peer.role != want) {
        meshSendSetRole(peer.mac, want);
      }
    }
  }

  const bool requested = server.hasArg("mode") || server.hasArg("led") || server.hasArg("bri") ||
                        server.hasArg("role");
  if (requested) {
    redirectControl();
    return;
  }
  sendControlPage();
}

static void setupWebUi() {
  server.on("/", HTTP_GET, []() { handleControl(); });
  server.on("/control", HTTP_GET, []() { handleControl(); });

  ElegantOTA.begin(&server, "ota", OTA_PASSWORD);
  ElegantOTA.onStart([]() {
    Serial.println("ElegantOTA start");
    applyLed();
    g_showInfo = true;
    g_infoUntilMs = millis() + 600000;
    drawInfoOverlay("ElegantOTA...");
  });
  ElegantOTA.onEnd([](bool success) {
    Serial.printf("ElegantOTA end success=%d\n", success);
    if (!success) {
      setLedRaw(CRGB(80, 0, 0));
      g_showInfo = true;
      g_infoUntilMs = millis() + 5000;
      drawInfoOverlay("OTA failed");
    } else {
      applyLed();
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
  Serial.printf(" channel : %d\n", WiFi.channel());
  Serial.printf(" role    : %s\n", roleName(g_role));
  Serial.printf(" viz     : %s\n", modeName(g_screenIndex));
  Serial.printf(" esp-now : %s\n", meshReady() ? "up" : "off");
  if (g_role == ROLE_CONDUCTOR) {
    Serial.printf(" audio   : udp://%s:%u\n", WiFi.localIP().toString().c_str(), kAudioUdpPort);
  }
  Serial.println(" display : LovyanGFX / ST7789 172x320");
  Serial.printf(" control : http://%s/control\n", WiFi.localIP().toString().c_str());
  Serial.printf(" ElegantOTA: http://%s/update  user=ota\n", WiFi.localIP().toString().c_str());
  Serial.println("========================================");
}

static void logLink() {
  static uint32_t last = 0;
  static uint32_t prevPkts = 0;
  const uint32_t now = millis();
  if (now - last < 3000) {
    return;
  }
  const uint32_t pkts = audioPackets();
  const uint32_t dt = now - last;
  const uint32_t rate = dt ? (pkts - prevPkts) * 1000 / dt : 0;
  prevPkts = pkts;
  last = now;
  Serial.printf("role=%s peers=%d udp=%u/s mesh=%s viz=%s\n", roleName(g_role), meshPeerCount(),
                rate, meshReady() ? "up" : "off", modeName(g_screenIndex));
}

static void noteFrame(const VizFrame &frame) {
  g_viz = frame;
}

static void publishViz() {
  if (g_role == ROLE_CONDUCTOR) {
    VizFrame frame;
    if (audioTake(&frame)) {
      noteFrame(frame);
      meshSendViz(frame);
    }
  } else {
    VizFrame frame;
    if (meshTakeViz(&frame)) {
      noteFrame(frame);
    }
  }
}

static void updateUi() {
  if (!g_display) {
    return;
  }
  const uint32_t now = millis();
  if (g_showInfo) {
    if ((int32_t)(now - g_infoUntilMs) >= 0) {
      g_showInfo = false;
      g_vizDirty = true;
    } else {
      return;
    }
  }

  static uint32_t lastFrame = 0;
  const bool timeForFrame = (now - lastFrame) >= kVizFrameMs;
  if (!g_vizDirty && !timeForFrame) {
    return;
  }

  const bool seqNew = g_viz.seq != g_drawnSeq;
  const bool spectrum = g_screenIndex == MODE_SPECTRUM;
  if (!g_vizDirty && !seqNew && !spectrum) {
    return;
  }
  lastFrame = now;

  const bool full = g_vizDirty;
  g_vizDirty = false;
  if (full) {
    clearVizArea();
    drawLufsChrome();
    g_specInit = false;
    g_waveValid = false;
    g_xyValid = false;
    g_specWrite = 0;
    g_lufsH[0] = g_lufsH[1] = g_lufsH[2] = -1;
  }

  tft.startWrite();
  switch (g_screenIndex) {
    case MODE_SPECTRUM:
      drawSpectrum();
      break;
    case MODE_STEREOGRAM:
      drawStereogram();
      break;
    case MODE_SPECTROGRAM:
      drawSpectrogramColumn();
      break;
    case MODE_WAVEFORM:
    case MODE_OSCILLOSCOPE:
    default:
      drawWaveform();
      break;
  }
  drawLufsBars();
  tft.endWrite();
  drawModeLabel();
  g_drawnSeq = g_viz.seq;
}

void setup() {
  Serial.begin(115200);
  delay(800);

  FastLED.addLeds<WS2812, RGB_LED_PIN, RGB>(leds, NUM_LEDS);
  FastLED.setBrightness(10);
  setLedRaw(CRGB::Black);

  buildHostname();
  loadSettings();
  applyLed();
  Serial.printf("Booting %s (%s) role=%s\n", hostname, kFirmwareVersion, roleName(g_role));

  setupDisplay();
  connectWifi();
  publishIdentity(false);
  meshBegin(onMeshConfig, onMeshClaim, onMeshMode, onMeshRole);
  audioSetVizMode((uint8_t)g_screenIndex);
  audioBegin(g_role == ROLE_CONDUCTOR);
  setupArduinoOta();
  setupWebUi();
  printBanner();

  applyLed();
#if MMM_PREFERRED_CONDUCTOR
  if (g_role == ROLE_CONDUCTOR) {
    g_claimGuardUntil = millis() + 3000;
    meshSendClaim();
  }
#endif
  if (g_display) {
    g_showInfo = true;
    g_infoUntilMs = millis() + kInfoOverlayMs;
    drawInfoOverlay(WiFi.localIP().toString().c_str());
  }
}

void loop() {
  serviceNet();
  const uint32_t now = millis();
  meshPollRx();
  resolveConductorRole(now);
  audioPoll(now);
  publishViz();
  meshPollTx(now);
  updateUi();
  logLink();
  serviceNet();
  delay(1);
}
