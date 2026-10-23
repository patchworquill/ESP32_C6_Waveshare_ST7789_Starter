# ESPokedex --- Project Brief

## Goal

Build firmware for the Waveshare **ESP32-C6-LCD-1.47** using native
**ESP-IDF**, with PlatformIO as the build workflow. Develop from either
Vim or VSCode.

## Current setup

The project folder is `ESPokedex`. Current `platformio.ini`:

``` ini
[platformio]
default_envs = esp32c6

[env:esp32c6]
platform = platformio/espressif32
board = esp32-c6-devkitm-1
framework = espidf

monitor_speed = 115200
upload_speed = 460800

build_type = debug
```

PlatformIO Core installed: **6.1.19**.

The board setting is a temporary 4 MB target for toolchain bring-up, not
a verified Waveshare board definition. Confirm actual flash
configuration and ESP-IDF/platform compatibility before finalizing the
project configuration.

## Hardware

-   ESP32-C6, 4 MB flash
-   1.47-inch ST7789 LCD, **172 × 320** pixels
-   LCD SPI: MOSI GPIO 6, SCLK GPIO 7, CS GPIO 14, DC GPIO 15, reset
    GPIO 21
-   Backlight GPIO 22
-   RGB LED GPIO 8
-   microSD/TF interface: CS GPIO 4, MOSI GPIO 6, SCLK GPIO 7, MISO GPIO
    5
-   BOOT and RESET buttons; verify BOOT GPIO and active level before
    implementing input handling
-   LCD and microSD share SPI signals and use separate chip-selects

Official documentation:
https://docs.waveshare.com/ESP32-C6-LCD-1.47?variant=ESP32-C6-LCD-1.47

## Required foundation

1.  Build and flash a minimal ESP-IDF application over USB; verify
    serial logging.
2.  Bring up the LCD and show a color calibration/test screen.
3.  Add RGB LED and button handling.
4.  Implement Wi-Fi provisioning: connect with saved credentials; if
    credentials are absent or connection recovery fails, start a setup
    AP and browser-based configuration page. Store credentials in NVS.
    Do not erase credentials just because a router is temporarily
    unavailable.
5.  Implement authenticated local-network OTA using ESP-IDF OTA APIs,
    two application slots, OTA metadata, image validation, and
    rollback/recovery where supported.
6.  Keep connectivity and OTA independent of the Pokédex UI so the
    device remains recoverable after a bad UI update.

## Pokédex application

-   Custom handheld-console GUI inspired by the supplied reference
    image; configurable main case color rather than fixed red.
-   Display Pokémon sprites at their original pixel dimensions on the
    172 × 320 screen.
-   Animate sprites and use the onboard button to advance through the
    National Pokédex index.
-   Set the RGB LED to each sprite's dominant color.
-   Create a build-time asset script to convert legally sourced sprite
    images, generate animation metadata, calculate/cache dominant RGB
    colors, and build a Pokédex catalog.
-   Start with the original 150 Pokémon. Measure generated asset size
    and firmware/flash usage before deciding whether sprites should be
    embedded in flash or stored on microSD.

## Suggested structure

``` text
ESPokedex/
├── platformio.ini
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── src/
│   └── main.cpp
├── components/
│   ├── board/
│   ├── connectivity/
│   ├── web_config/
│   ├── ota/
│   └── pokedex/
├── assets/
│   ├── source/
│   └── generated/
└── scripts/
    └── generate_assets.py
```

Treat this as a proposed structure; create files as needed and keep the
first milestone minimal.

## Development guidance

-   Use native ESP-IDF components/APIs, with C++ for application code
    where convenient; do not switch to Arduino framework.
-   Reuse and verify the official Waveshare ESP-IDF demo/driver before
    writing a display driver from scratch:
    https://docs.waveshare.com/ESP32-C6-LCD-1.47/ESP-IDF
-   A useful independent LCD bring-up reference is
    https://github.com/rickenator/waveshare-esp32-c6-1.47-demo
-   Plan and verify the 4 MB partition table before implementing OTA.
    Leave room for bootloader, partition table, NVS, OTA metadata, and
    two application slots.
-   Keep dependencies and toolchain versions reproducible in Git. Do not
    commit Wi-Fi credentials or other secrets.

## Immediate next task for the Copilot agent

Inspect the existing project and the official Waveshare ESP-IDF example.
Implement the smallest buildable PlatformIO/ESP-IDF starter that
compiles and flashes, then bring up serial logging and the LCD test
screen. Verify pin mappings against the official documentation before
coding. Do not implement all application features at once; proceed
milestone by milestone and report any assumptions or hardware details
that still need verification.
