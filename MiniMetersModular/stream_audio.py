#!/usr/bin/env python3
"""Stream real PCM to a MiniMeters conductor over UDP.

The conductor (set on /control) listens on UDP 42100 for 16 kHz int16
frames. This script captures the default input device and resamples to
that rate. Pass --tone to send a sine instead, with no extra packages.

  pip install sounddevice
  python stream_audio.py 192.168.1.224
  python stream_audio.py 192.168.1.224 --tone 440
"""

from __future__ import annotations

import argparse
import math
import socket
import struct
import sys
import time

MAGIC = b"MMA1"
RATE = 16000
HEADER = struct.Struct("<4sIHHHH")


def resample(samples: list[int], channels: int, src_rate: int) -> list[int]:
    if src_rate == RATE:
        return samples
    in_frames = len(samples) // channels
    if in_frames <= 1:
        return []
    out_frames = max(1, int(in_frames * RATE / src_rate))
    out: list[int] = []
    for i in range(out_frames):
        pos = i * src_rate / RATE
        i0 = int(pos)
        if i0 >= in_frames:
            i0 = in_frames - 1
        i1 = i0 + 1 if i0 + 1 < in_frames else i0
        frac = pos - i0
        for ch in range(channels):
            a = samples[i0 * channels + ch]
            b = samples[i1 * channels + ch]
            out.append(int(a + (b - a) * frac))
    return out


def apply_gain(samples: list[int], gain: float) -> list[int]:
    if gain == 1.0:
        return samples
    scaled = []
    for sample in samples:
        v = int(sample * gain)
        if v > 32767:
            v = 32767
        elif v < -32768:
            v = -32768
        scaled.append(v)
    return scaled


def pack_audio(seq: int, samples: list[int], channels: int) -> bytes:
    count = len(samples) // channels
    body = struct.pack("<" + "h" * len(samples), *samples)
    return HEADER.pack(MAGIC, seq & 0xFFFFFFFF, RATE, channels, count, 0) + body


def peak_of(samples: list[int]) -> int:
    peak = 0
    for sample in samples:
        a = -sample if sample < 0 else sample
        if a > peak:
            peak = a
    return peak


def run_tone(sock: socket.socket, host: str, port: int, freq: float, gain: float) -> None:
    channels = 2
    frames = 256
    seq = 0
    phase = 0.0
    step = 2.0 * math.pi * freq / RATE
    interval = frames / RATE
    next_t = time.perf_counter()
    sent = 0
    last_log = time.perf_counter()
    print(f"tone {freq:.1f} Hz -> udp://{host}:{port}")
    while True:
        samples: list[int] = []
        for _ in range(frames):
            left = int(12000 * gain * math.sin(phase))
            right = int(9000 * gain * math.sin(phase + 0.7))
            if left > 32767:
                left = 32767
            if right > 32767:
                right = 32767
            samples.append(left)
            samples.append(right)
            phase += step
            if phase > 2.0 * math.pi:
                phase -= 2.0 * math.pi
        sock.sendto(pack_audio(seq, samples, channels), (host, port))
        seq += 1
        sent += 1
        now = time.perf_counter()
        if now - last_log >= 1.0:
            print(f"sent {sent} pkts  peak {peak_of(samples)}")
            sent = 0
            last_log = now
        next_t += interval
        delay = next_t - time.perf_counter()
        if delay > 0:
            time.sleep(delay)
        else:
            next_t = time.perf_counter()


def open_input():
    import sounddevice as sd

    info = sd.query_devices(kind="input")
    native = int(info["default_samplerate"])
    max_ch = int(info["max_input_channels"])
    if max_ch < 1:
        raise RuntimeError(f"no input channels on {info['name']}")
    attempts = [(RATE, min(2, max_ch)), (native, min(2, max_ch)), (native, 1)]
    last: Exception | None = None
    for rate, ch in attempts:
        if rate < 8000 or ch < 1:
            continue
        block = max(1, int(256 * rate / RATE))
        try:
            stream = sd.InputStream(
                samplerate=rate,
                channels=ch,
                dtype="int16",
                blocksize=block,
                latency="low",
            )
            stream.start()
            print(f"input: {info['name']}  {rate} Hz {ch} ch -> {RATE} Hz")
            return stream, rate, ch
        except Exception as exc:
            last = exc
    raise RuntimeError(f"could not open the input device: {last}")


def run_capture(sock: socket.socket, host: str, port: int, gain: float) -> None:
    stream, src_rate, channels = open_input()
    seq = 0
    sent = 0
    last_log = time.perf_counter()
    overflow_noted = False
    print(f"streaming -> udp://{host}:{port}")
    try:
        while True:
            data, overflowed = stream.read(stream.blocksize)
            if overflowed and not overflow_noted:
                print("input overflow — the capture buffer slipped")
                overflow_noted = True
            raw = data.tobytes()
            samples = list(struct.unpack("<" + "h" * (len(raw) // 2), raw))
            samples = resample(samples, channels, src_rate)
            samples = apply_gain(samples, gain)
            if len(samples) < channels:
                continue
            # Keep packets near 256 frames so the conductor's FFT window fills evenly.
            frame = 256 * channels
            offset = 0
            while offset + channels <= len(samples):
                end = offset + frame
                if end > len(samples):
                    end = len(samples) - ((len(samples) - offset) % channels)
                chunk = samples[offset:end]
                if len(chunk) < channels:
                    break
                sock.sendto(pack_audio(seq, chunk, channels), (host, port))
                seq += 1
                sent += 1
                offset = end
            now = time.perf_counter()
            if now - last_log >= 1.0:
                print(f"sent {sent} pkts  peak {peak_of(samples)}")
                sent = 0
                last_log = now
    finally:
        stream.stop()
        stream.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Stream PCM to a MiniMeters conductor")
    parser.add_argument("host", help="conductor IP, shown on its /control page")
    parser.add_argument("--port", type=int, default=42100)
    parser.add_argument("--tone", type=float, default=0, help="Hz sine instead of the mic")
    parser.add_argument("--gain", type=float, default=1.0)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        if args.tone > 0:
            run_tone(sock, args.host, args.port, args.tone, args.gain)
        else:
            try:
                import sounddevice  # noqa: F401
            except ImportError:
                print("Install the capture package:  pip install sounddevice", file=sys.stderr)
                print("Or pass --tone 440 to stream a test sine.", file=sys.stderr)
                return 1
            run_capture(sock, args.host, args.port, args.gain)
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
