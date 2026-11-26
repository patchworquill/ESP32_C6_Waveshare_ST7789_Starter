# MiniMeters Modular

Embedded MiniMeters-style audio meters on ESP32 and nRF devkits, with common SPI displays (ILI9341, ST7789, GC9A01).

One node is the conductor. It takes 16 kHz PCM over UDP, runs the FFT and loudness, and broadcasts a small frame over ESP-NOW. Every other node renders that frame. Any node's `/control` page can push the visualizer, LED color, and LED brightness to the rest of the mesh. A DAC on each node is still planned.

ESP-NOW rides the Wi-Fi radio, so both boards have to join the same access point (they already do). The C6's 802.15.4 radio can run Thread later for a cable-free audio fabric; viz frames and config fit in ESP-NOW's 250-byte payload, which keeps the web UI and OTA on the stack that is already working.

Visualizations (same idea as the MiniMeters app):

- Spectrum
- LUFS Meter
- Stereogram
- Spectrogram
- Waveform
- Oscilloscope

The conductor's FFT and K-weighted loudness live in `src/audio_in.cpp`. [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools.git) is the planned helper for the USB and DAC path.

Related reading:

- https://www.dellascolto.com/bitwise/2022/08/14/wireless-audio-visualizer_part_i/
- https://www.dellascolto.com/bitwise/2022/08/14/wireless-audio-visualizer-part-ii/

## Current milestone: mesh meters

Both Waveshare ESP32-C6 boards run one shared firmware image:

- **ESP-NOW mesh** for configuration and visualizer frames
- **UDP audio** into the conductor (`stream_audio.py`)
- **ArduinoOTA** and **ElegantOTA** as before

### Mesh and audio

1. Flash both boards (below). On each `/control` page, set **one** node to Conductor. That demotes the other.
2. Stream audio at the conductor. Its page shows the URL:

   ```bash
   pip install sounddevice
   python stream_audio.py 192.168.1.224
   ```

   `python stream_audio.py 192.168.1.224 --tone 440` sends a stereo sine if you do not have a capture device yet.

3. Change the visualizer, LED color, or LED brightness on **either** node's `/control` page. The change is stored in NVS and broadcast. A node that boots later picks up the highest settings revision from the next hello.

Wi-Fi modem sleep is off while this firmware runs. ESP-NOW drops frames when the STA radio is dozing, and these boards are on USB power.

The display and the web server share one core. Each frame is an incremental update capped at about 20 fps (`kVizFrameMs` in `src/main.cpp`), and the loop gives the TCP stack a 1 ms slot every pass. Tall meters used to repaint their full height every frame, which is why the page stalled once the LUFS bars filled in. Lower `kVizFrameMs` if you want smoother motion and the page still answers quickly.

### OTA

- **ArduinoOTA** from PlatformIO (`pio run -e ota -t upload --upload-port <ip>`)
- **ElegantOTA** at `http://<ip>/update` (user `ota`, password from secrets)

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

Onboard RGB: solid color from `/control` (default dim orange-red), saved on the module. The boot screen shows firmware version, hostname, role, and IP.

Display stack: **LovyanGFX** + ST7789 (Waveshare 1.47" 172×320). ~90% TFT_eSPI-compatible API; works on ESP32 Arduino 3.x / C6 where upstream TFT_eSPI still struggles ([PR #3399](https://github.com/Bodmer/TFT_eSPI/pull/3399)). Board config lives in `include/LGFX_Waveshare_C6_147.hpp`.

### Wireless update

**ArduinoOTA (PlatformIO)** — use the IP or `mmm-xxxx.local` from the serial banner:

```bash
pio run -e ota -t upload --upload-port 192.168.1.42
```

**ElegantOTA:** open `http://<ip>/update`, sign in (`ota` / `OTA_PASSWORD`), upload `.pio/build/waveshare_c6/firmware.bin`.
