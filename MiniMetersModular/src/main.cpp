#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ElegantOTA.h>

#include "LGFX_Waveshare_C6_147.hpp"
#include "secrets.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0-dev"
#endif

static const char *kFirmwareVersion = FIRMWARE_VERSION;

static WebServer server(80);
static LGFX tft;

#define RGB_LED_PIN 8

// Idle identity color for this build (aqua)
static const uint8_t kAquaR = 0;
static const uint8_t kAquaG = 200;
static const uint8_t kAquaB = 200;

static char hostname[32];

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_LED_PIN, r, g, b);
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
  tft.print("LED = aqua  GFX = Lovyan");
  tft.setCursor(8, 268);
  tft.print("OTA portal: /update");
}

static void setupDisplay() {
  tft.init();
  tft.setRotation(1);  // USB connector on the right
  tft.setBrightness(230);
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
    setLed(kAquaR / 3, kAquaG / 3, kAquaB / 3);
    delay(50);
    setLed(0, 0, 0);
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
    setLed(255, 120, 0);
    drawStatusScreen("ArduinoOTA...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nArduinoOTA end");
    setLed(kAquaR, kAquaG, kAquaB);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("ArduinoOTA progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("ArduinoOTA error[%u]\n", error);
    setLed(255, 0, 0);
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
    setLed(160, 0, 200);
    drawStatusScreen("ElegantOTA...");
  });
  ElegantOTA.onEnd([](bool success) {
    Serial.printf("ElegantOTA end success=%d\n", success);
    setLed(success ? kAquaR : 255, success ? kAquaG : 0, success ? kAquaB : 0);
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
  if (on) {
    setLed(kAquaR, kAquaG, kAquaB);
  } else {
    setLed(0, 0, 0);
  }
}

void setup() {
  Serial.begin(115200);
  delay(800);

  setLed(0, 0, 40);
  buildHostname();
  Serial.printf("Booting %s (%s)\n", hostname, kFirmwareVersion);

  setupDisplay();
  connectWifi();
  setupArduinoOta();
  setupElegantOta();
  printBanner();

  drawStatusScreen(WiFi.localIP().toString().c_str());
  setLed(kAquaR, kAquaG, kAquaB);
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  ElegantOTA.loop();
  rgbHeartbeat();
}
