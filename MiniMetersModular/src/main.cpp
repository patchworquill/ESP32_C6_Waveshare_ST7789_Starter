#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ElegantOTA.h>
#include <FastLED.h>

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

// Reddish-orange at ~10% of full scale (logical RGB)
static const CRGB kIdleColor = CRGB(255, 69, 0);

#define SMOOTHERSTEP(x) ((x) * (x) * (x) * ((x) * ((x) * 6 - 15) + 10))  // https://en.wikipedia.org/wiki/Smoothstep#Variations

// Display backlight: default 30%, hard cap 50%
static const uint8_t kDisplayBrightnessPercent = 30;
static const uint8_t kDisplayBrightnessMaxPercent = 50;

static char hostname[32];

float inverse_smoothstep(float x) {               // https://en.wikipedia.org/wiki/Smoothstep#Variations
  return 0.5 - sin(asin(1 - 2 * x) / 3);
}

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

static void drawStatusScreen(const char *wifiLine) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(false);

  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.setCursor(8, 12);
  tft.print("MiniMeters");
  tft.setCursor(8, 36);
  tft.print("Modular");

  tft.setTextColor(TFT_DARKGREY);
  tft.setTextSize(1);
  tft.setCursor(8, 72);
  tft.print("firmware");

  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(8, 90);
  tft.print(kFirmwareVersion);

  tft.setTextColor(TFT_DARKGREY);
  tft.setTextSize(1);
  tft.setCursor(8, 128);
  tft.print("hostname");

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 146);
  tft.print(hostname);

  tft.setTextColor(TFT_DARKGREY);
  tft.setTextSize(1);
  tft.setCursor(8, 184);
  tft.print("network");

  tft.setTextColor(TFT_GREEN);
  tft.setTextSize(2);
  tft.setCursor(8, 202);
  tft.print(wifiLine);

  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(1);
  tft.setCursor(8, 250);
  tft.print("LED = orange@10%  FastLED");
  tft.setCursor(8, 268);
  tft.print("OTA portal: /update");
}

static void setupDisplay() {
  tft.init();
  tft.setRotation(1);  // USB connector on the right
  tft.setBrightness(displayBrightnessByte(kDisplayBrightnessPercent));
  drawStatusScreen("connecting...");
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
      drawStatusScreen("wifi retry...");
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
    drawStatusScreen("ArduinoOTA...");
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
    drawStatusScreen("OTA error");
  });

  ArduinoOTA.begin();
  Serial.println("ArduinoOTA ready");
}

static void setupElegantOta() {
  server.on("/", HTTP_GET, []() {
    String body;
    body.reserve(256);
    body += "MiniMetersModular OTA baseline\n";
    body += "version: ";
    body += kFirmwareVersion;
    body += "\nhostname: ";
    body += hostname;
    body += "\nip: ";
    body += WiFi.localIP().toString();
    body += "\n\nElegantOTA portal: /update\n";
    server.send(200, "text/plain", body);
  });

  ElegantOTA.begin(&server, "ota", OTA_PASSWORD);
  ElegantOTA.onStart([]() {
    Serial.println("ElegantOTA start");
    setLed(kIdleColor);
    drawStatusScreen("ElegantOTA...");
  });
  ElegantOTA.onEnd([](bool success) {
    Serial.printf("ElegantOTA end success=%d\n", success);
    setLed(success ? kIdleColor : CRGB(25, 0, 0));
    if (!success) {
      drawStatusScreen("OTA failed");
    }
  });

  server.begin();
  Serial.println("ElegantOTA ready at /update");
}

static void printBanner() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" MiniMetersModular OTA baseline");
  Serial.printf(" version : %s\n", kFirmwareVersion);
  Serial.printf(" hostname: %s\n", hostname);
  Serial.printf(" ip      : %s\n", WiFi.localIP().toString().c_str());
  Serial.println(" display : LovyanGFX / ST7789 172x320");
  Serial.println(" ArduinoOTA password: (see secrets.h OTA_PASSWORD)");
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

void setup() {
  Serial.begin(115200);
  delay(800);

  // Waveshare C6 onboard LED is WS2812 with RGB wire order (GRB showed green-yellow for orange).
  FastLED.addLeds<WS2812, RGB_LED_PIN, RGB>(leds, NUM_LEDS);
  FastLED.setBrightness(10);
  setLed(CRGB::Black);

  buildHostname();
  Serial.printf("Booting %s (%s)\n", hostname, kFirmwareVersion);

  setupDisplay();
  connectWifi();
  setupArduinoOta();
  setupElegantOta();
  printBanner();

  drawStatusScreen(WiFi.localIP().toString().c_str());
  setLed(kIdleColor);
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  ElegantOTA.loop();
  rgbHeartbeat();
}
