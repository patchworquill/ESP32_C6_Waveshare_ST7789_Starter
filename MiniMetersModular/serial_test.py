#!/usr/bin/env python3
"""Read boot banners from all /dev/cu.usbmodem* ports.

By default does NOT toggle RTS (ESP32-C6 native USB often drops after reset).
Pass --reset to force a hardware reset before reading.
"""

from __future__ import annotations

import glob
import sys
import time

import serial


def ports() -> list[str]:
    found = sorted(glob.glob("/dev/cu.usbmodem*"))
    return found


def read_port(port: str, seconds: float = 10.0, reset: bool = False) -> None:
    print(f"==== {port} ====")
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
        if reset:
            ser.dtr = False
            ser.rts = True
            time.sleep(0.1)
            ser.rts = False
        deadline = time.time() + seconds
        while time.time() < deadline:
            data = ser.read(512)
            if data:
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()
        ser.close()
    except Exception as e:
        print("ERR", e)
    print()


def main() -> int:
    reset = "--reset" in sys.argv
    found = ports()
    if not found:
        print("No /dev/cu.usbmodem* ports found.")
        print("Unplug/replug the missing board, or Boot+Reset.")
        return 1
    print(f"Found: {', '.join(found)}")
    if not reset:
        print("(listening only — use --reset to toggle RTS)\n")
    for port in found:
        read_port(port, reset=reset)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
