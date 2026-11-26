# MiniMeters Modular

Embedded MiniMeters-style audio meters on ESP32 and nRF devkits, with common SPI displays (ILI9341, ST7789, GC9A01).

Audio moves between nodes over a Thread mesh (no audio cables). Each node can also drive a DAC for local output.

Visualizations (same idea as the MiniMeters app):

- Spectrum
- LUFS Meter
- Stereogram
- Spectrogram
- Waveform
- Oscilloscope

Streaming / audio helpers come from [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools.git) (git submodule).

Related reading:

- https://www.dellascolto.com/bitwise/2022/08/14/wireless-audio-visualizer_part_i/
- https://www.dellascolto.com/bitwise/2022/08/14/wireless-audio-visualizer-part-ii/

## Current milestone: OTA baseline

Both Waveshare ESP32-C6 boards run a shared PlatformIO firmware with:

- **ArduinoOTA** — `pio run -e ota_1101 -t upload` (or `ota_101`)
- **ElegantOTA** — browser portal at `http://<ip>/update` (user `ota`, password from secrets)

### First flash (USB)

Boards are identical — **build once, flash twice** with the same `firmware.bin`. Hostnames are `mmm-<chip-suffix>` from efuse MAC so the two nodes still distinguish themselves on the LAN.

1. Copy secrets and fill in Wi‑Fi + OTA password:

   ```bash
   cp include/secrets.h.example include/secrets.h
   ```

2. Keep `upload_flags --auth=` in `[env:ota]` in sync with `OTA_PASSWORD` in `secrets.h`.

3. Build once, then flash both ports (Boot+Reset if a port does not appear):

   ```bash
   pio run
   pio run -t upload --upload-port /dev/cu.usbmodem1101
   pio run -t upload --upload-port /dev/cu.usbmodem101
   ```

4. Monitor and note each board’s IP / hostname:

   ```bash
   pio device monitor --port /dev/cu.usbmodem1101
   ```

Onboard RGB: **aqua heartbeat** while idle (this build’s visual marker). Display shows firmware version, hostname, and IP.

Display stack: **LovyanGFX** + ST7789 (Waveshare 1.47" 172×320). ~90% TFT_eSPI-compatible API; works on ESP32 Arduino 3.x / C6 where upstream TFT_eSPI still struggles ([PR #3399](https://github.com/Bodmer/TFT_eSPI/pull/3399)). Board config lives in `include/LGFX_Waveshare_C6_147.hpp`.

### Wireless update

**ArduinoOTA (PlatformIO)** — use the IP or `mmm-xxxx.local` from the serial banner:

```bash
pio run -e ota -t upload --upload-port 192.168.1.42
```

**ElegantOTA:** open `http://<ip>/update`, sign in (`ota` / `OTA_PASSWORD`), upload `.pio/build/waveshare_c6/firmware.bin`.
