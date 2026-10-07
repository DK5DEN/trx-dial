#!/usr/bin/env python3
"""Grab the current dial frame over USB serial: python shot.py COM5 out.png [cmd ...]

The firmware answers "shot" with "RAW w h", base64 lines of the RGB565
frame buffer (big-endian per pixel, as stored by LovyanGFX sprites) and "END".
"""
import base64
import struct
import sys
import time
import zlib

import serial


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


port, out = sys.argv[1], sys.argv[2]
s = serial.Serial()
s.port, s.baudrate, s.timeout, s.write_timeout = port, 115200, 2, 2
s.dtr = False   # opening must not reset the ESP32-S3
s.rts = False
s.open()
time.sleep(0.3)
s.reset_input_buffer()
for cmd in sys.argv[3:]:   # optional input before the shot, e.g. "rot 2" click long
    s.write(cmd.encode() + b"\n")
    time.sleep(0.4)
s.write(b"shot\n")
w = h = None
data = b""
deadline = time.time() + 15
while time.time() < deadline:
    line = s.readline().strip()
    if line.startswith(b"RAW "):
        w, h = map(int, line.split()[1:3])
    elif line == b"END":
        break
    elif w and line:
        data += base64.b64decode(line)
s.close()
if not w or len(data) != w * h * 2:
    sys.exit(f"incomplete frame: {len(data)} bytes")
rgb = bytearray()
for i in range(0, len(data), 2):
    v = (data[i] << 8) | data[i + 1]
    r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    rgb += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
write_png(out, w, h, bytes(rgb))
print(out)
