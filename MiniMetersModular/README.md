# MiniMeters Modular

Embedded MiniMeters-style audio meters on ESP32 and nRF devkits, with common SPI displays (ILI9341, ST7789, GC9A01).

One node is the conductor. It takes 16 kHz PCM over UDP, runs the FFT and loudness, and broadcasts a small frame over ESP-NOW. Every other node renders that frame in its own visualizer. Theme, LED color, and brightness are shared. Any node's page can set those, and can set each node's visualizer and role from that node's card. A DAC on each node is still planned. The ESP32-S3 build is the headless conductor.

ESP-NOW rides the Wi-Fi radio, so both boards have to join the same access point (they already do). The C6's 802.15.4 radio can run Thread later for a cable-free audio fabric; viz frames and config fit in ESP-NOW's 250-byte payload, which keeps the web UI and OTA on the stack that is already working.

Visualizations (same idea as the MiniMeters app):

- Spectrum
- LUFS Meter
- Stereogram
- Spectrogram
- Waveform
- Oscilloscope

The conductor's FFT and K-weighted loudness live in `src/audio_in.cpp`. Feed it with `stream_audio.py` over UDP. USB audio on the S3 enumerates and does not stream; that work is parked in `TODO.md`. A codec on the I2S pins is the next input.

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

3. Open `/` or `/control`. Each node is a card. The card you are on is marked **this node**. Pick that card's visualizer and role there. **Off** blanks a display. A node with no panel shows those choices greyed out. Theme, LED color, and brightness sit under the cards and apply to every node. Themes come from `themes/*.ini` (same MiniMeters palettes). Hover **status** on a card for firmware, address, role, and visualizer. Each card links to that node's control page and `/update`.

### ESP32-S3 conductor

The S3 target is a Seeed XIAO ESP32-S3. It has no SPI panel, so it boots headless and still joins Wi-Fi, the mesh, and the web page. It defaults to conductor. Its visualizer controls are greyed out.

```bash
pio run -e xiao_s3 -t upload
```

Flash every node when you move to this firmware. The mesh protocol is version 4, and a board still on an older build will not see the others.

The USB-C port is the serial log. A USB speaker endpoint is compiled in and is not a working interface; see `TODO.md`. Stream 16 kHz PCM at the conductor:

```bash
python stream_audio.py 192.168.1.224
```

The red LED next to the USB jack is the battery charger. It stays on while the board is bus-powered and does not follow the web page. The user LED is the yellow one on GPIO21. It is a single color, so the shared color control changes its brightness.

After this firmware enumerates, the serial port name can change. If the monitor does not attach, pick the new `cu.usbmodem` port.

Wi-Fi modem sleep is off while this firmware runs. ESP-NOW drops frames when the STA radio is dozing, and these boards are on USB power.

The display and the web server share one core. Each frame is an incremental update capped at about 20 fps (`kVizFrameMs` in `src/main.cpp`), and the loop gives the TCP stack a 1 ms slot every pass. Tall meters used to repaint their full height every frame, which is why the page stalled once the LUFS bars filled in. Lower `kVizFrameMs` if you want smoother motion and the page still answers quickly.

### OTA

- **ArduinoOTA** from PlatformIO (`pio run -e ota -t upload --upload-port <ip>` for a C6, `pio run -e ota_s3 -t upload --upload-port <ip>` for the S3)
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
pio run -e ota_s3 -t upload --upload-port 192.168.1.230
```

`ota` is the C6 image. `ota_s3` is the XIAO image. Point each one at that board's IP.

**ElegantOTA:** open `http://<ip>/update`, sign in (`ota` / `OTA_PASSWORD`), and upload `.pio/build/waveshare_c6/firmware.bin` or `.pio/build/xiao_s3/firmware.bin`.
