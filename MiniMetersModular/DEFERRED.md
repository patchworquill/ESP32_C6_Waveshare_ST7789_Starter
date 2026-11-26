# Parked: S3 USB audio

Do not keep tuning the USB speaker. It enumerates on macOS and then fails as an interface: glitches, no sample throughput, and the whole device (serial included) leaves the bus when the stream is selected. UDP into the conductor is the working input. Next host-side audio should be an I2S codec, then TDM if a multi-channel part is actually required. Bluetooth Classic is not available on these chips.

## What is on the S3 right now

`[env:xiao_s3]` / `[env:ota_s3]`, firmware 1.5.0, mesh v3. Headless Seeed XIAO ESP32-S3.

`usbAudioStart()` in `src/audio_in.cpp` builds a composite CDC + UAC2 device named MiniMeters (`vid 0x303A`, `pid 0x4002`, serial `mmm-%04x`). Current shape, after the failed attempts below:

- Speaker only (`RX_MODE`). The microphone endpoint is off.
- Adaptive sync. `enable_feedback_ep` is false, so there is no explicit feedback endpoint.
- 48 kHz, 16-bit stereo. `calcPacketSizeForRate()` in the audio-tools descriptor builder advertises an exact 192-byte frame when the rate divides 1 ms. The library's usual +1 sample (196 bytes) is skipped for those rates.
- Samples are decimated 3:1 in `drainUsb()` down to the existing 16 kHz FFT / LUFS path (`kAudioRate` in `include/mmm_proto.h`). Decimation runs in `loop()` after packets have already arrived. It cannot be why the device disappears. `usb=0/s` in the status line means `drainUsb()` never counted a packet.
- Web UI source pills (Auto / UDP / USB) and `MESH_SET_INPUT` are real. `mesh set input 2` on the serial log is the UI selecting USB. It does not mean macOS opened the stream.

Serial is a second interface on the same USB device, not a baud-rate limit. `include/mmm_log.h` remaps `Serial` to a `USBCDC`. Do not `printf` from the TinyUSB task; that deadlocks the CDC write against `tud_task`. The stream-open counter (`g_usbItfSets`) is incremented in the USB callback and printed from `audioPoll()`.

`lib/audio-tools` is a symlink to the `arduino-audio-tools` submodule. PlatformIO compiles that tree. A `lib_deps` copy under `.pio/libdeps/*/audio-tools` goes stale and will silently rebuild the old descriptors. The feedback `wMaxPacketSize` edit was lost that way once.

## What macOS actually did

Confirmed with `ioreg` on the machine that was playing audio:

- Full speed, 12 Mb/s. The S3 PHY is not high speed. Device class `239/2/1` (interface association). Configuration 1 selected, 250 mA.
- CDC came up: interface 0 control, interface 1 data, `/dev/cu.usbmodemmmm_db1c1`.
- UAC2 (`bInterfaceProtocol` 32) came up and `usbaudiod` attached. Idle alternate setting 0 reports `bNumEndpoints = 0`. That is normal. Endpoints appear only when CoreAudio selects the device.
- Duplex firmware showed two streaming interfaces (3 and 4). Speaker-only firmware showed one (interface 3). Both still failed when selected.
- Failure looks like: the device appears, passes no audio, then the USB device node disappears. `screen` dies with `%` because the tty went away. The status line keeps printing `usb=0/s` until that moment. No Guru Meditation was captured, so this is the USB device leaving the bus, not a panic that made it to the log.
- eqMac's "Restart Audio Service" dialog is CoreAudio losing the device. It is not a separate eqMac bug. Pick devices in System Settings > Sound.

## Attempts that did not produce a stream

1. 16 kHz UAC2. Enumerates, then macOS removes it when selected.
2. 48 kHz, explicit feedback, 10.14 format-correction callback returning true. The first OTA still had a 4-byte feedback `wMaxPacketSize` because it compiled `.pio/libdeps`. A later OTA compiled the submodule's 3-byte descriptor. macOS still removed the device. TinyUSB's own table (`audiod_fb_send` in `USBAudioDeviceBase.h`) says full-speed macOS wants descriptor size 3. Matching that was not sufficient.
3. Speaker only, to drop the third isochronous endpoint. The S3 DWC2 FIFO is 1024 bytes (`ep_fifo_size` in TinyUSB's `dwc2_esp32.h`; `OTG_DFIFO_DEPTH` 256 words). Endpoint numbers are EP0–EP5, five IN endpoints, and IN EP5 has no TX FIFO. Arduino CDC reserves OUT 3, IN 4, and IN 5. Speaker-only was confirmed live (one streaming interface) and still dropped the bus. `usb=0/s`.
4. Adaptive 48 kHz speaker, feedback endpoint removed, exact 192-byte packets. Still glitching, still no throughput.

Hardware context if anyone reopens this: this core is TinyUSB 0.17. `TUP_DCD_EDPT_ISO_ALLOC` is set. IN FIFO depth is doubled when `TXFELVL` is half-empty. `audiod_open()` still calls `isoAllocEndpoint(feedback, 4)` even when the descriptor says 3 bytes. A clean-room test would be Espressif's `usb_device_uac` example, not another flag in arduino-audio-tools. Do not start that unless someone asks.

Chris Greening (atomic14, Sep 2025, https://www.atomic14.com/2025/09/26/esp32-s3-usb-uac) could not get the Arduino libraries to work and switched to that ESP-IDF component. That gap was real at the time: on 29 Oct 2025 the audio-tools author said ESP32 USB audio was not working yet, because TinyUSB on ESP32 is wired differently from the other ports. The UAC2 API this tree uses landed later (announced 22 Jun 2026). Our submodule is `v1.3.0-27-gd73a95340` (25 Nov 2026), so we are already on the post-gap library. His "Support macOS" checkbox is `CONFIG_UAC_SUPPORT_MACOS`: 10.14 feedback so CoreAudio accepts the device, at the cost of Windows. The local 3-byte `wMaxPacketSize` edit is that same switch, and it still did not produce samples. His ESP-IDF result was also split by direction: the microphone streamed cleanly, and the speaker played but stayed crackly (direct I2S WAV on the same board was clean). Espressif issues IDFGH-15954 and esp-iot-solution #525 describe the same speaker-path glitches. Our use case is that speaker direction, and our symptom is worse than crackle: the device leaves the bus and `usb=0/s`. That is not a small descriptor patch. `audiod_open()` still calls `isoAllocEndpoint(..., 4)` for feedback; worth fixing if this is reopened, not sufficient on its own. The library docs say Linux is what was tested, and macOS is "designed to" work.

## Next audio inputs

Keep `stream_audio.py` / UDP as the conductor input until a codec is on the bench. The meters stay at 16 kHz. A 48 kHz codec still has to be decimated or the FFT has to be retuned. Do not change `kAudioRate` as a side effect of bringing up I2S.

**I2S codec on the XIAO S3.** This is the path to build. The S3 image is headless, so the display SPI pins are free, and I2S can be routed to any GPIO. No pins are assigned yet. A stereo ADC (PCM1808, ES8388, or an I2S mic) feeds `feedSample()`. A DAC (PCM5102, MAX98357) is the monitor output and is still the open "path onward to a DAC" item. Bring up one direction at a time, input first, because that is what the meters need.

**TDM.** The S3 I2S peripheral can do TDM. Use it when the part is a multi-channel codec, then downmix to the stereo 16 kHz analysis path. It is not a different transport. Do not start here for a stereo line-in.

**Bluetooth.** Neither the S3 nor the C6 has Bluetooth Classic, so A2DP is not possible on this hardware or on Arduino-ESP32 3.0.2 as built here. LE Audio is not a practical path on this core. A Bluetooth source means a different radio (original ESP32) or an external audio module. Do not spend a cycle trying `BluetoothA2DP` on the S3.

**Thread / 802.15.4.** Still the plan for audio between nodes, once ESP-NOW viz frames are not enough. That is a fabric between boards, not a way for the Mac to send audio. The C6 has the 802.15.4 radio. The S3 does not.

## Leave in place

Do not delete the USB device, the source pills, or `MMM_USB_AUDIO` unless asked. Serial CDC has to stay, because that is how the S3 is flashed and logged. The red LED by the USB jack is the charger. The user LED is GPIO21, active low, single color.
