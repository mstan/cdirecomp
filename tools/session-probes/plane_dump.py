#!/usr/bin/env python3
"""Dump plane A, plane B, and the composition-verdict map as PNGs.

Usage: py -3 tools/session-probes/plane_dump.py --port 4382 --prefix build/tmp/planes
Writes <prefix>-a.png, <prefix>-b.png, <prefix>-v.png (verdict colorized:
red=front(A over), green=back(B shown), blue=backdrop, gray=mixed).
"""
import argparse
import struct
import sys
import zlib

sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 2)[0])
from cdi_debug import send  # noqa: E402


def write_png(path, width, height, rows):
    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d +
                struct.pack(">I", zlib.crc32(t + d)))
    raw = b"".join(b"\x00" + r for r in rows)
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4382)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--prefix", default="build/tmp/planes")
    ap.add_argument("--height", type=int, default=240)
    a = ap.parse_args()

    width = None
    for plane, tag in ((0, "a"), (1, "b")):
        rows = []
        for y in range(a.height):
            r = send(a.host, a.port, {"cmd": "video_plane", "plane": plane,
                                      "y": y})
            if not r.get("ok"):
                print("row fail", plane, y, r)
                return
            width = r["width"]
            px = bytes.fromhex(r["argb"])
            rows.append(bytes(b for i in range(0, len(px), 4)
                              for b in px[i + 1:i + 4]))
        write_png(f"{a.prefix}-{tag}.png", width, a.height, rows)
        print(f"wrote {a.prefix}-{tag}.png")

    colors = {4: (255, 60, 60), 8: (60, 220, 60), 16: (60, 60, 255),
              32: (200, 200, 200)}
    rows = []
    for y in range(a.height):
        r = send(a.host, a.port, {"cmd": "video_plane", "plane": 2, "y": y})
        vb = bytes.fromhex(r["verdict"])
        row = bytearray()
        for v in vb:
            c = colors.get(v & 0x3C, (0, 0, 0))
            row.extend(c)
        rows.append(bytes(row))
    write_png(f"{a.prefix}-v.png", width, a.height, rows)
    print(f"wrote {a.prefix}-v.png")


if __name__ == "__main__":
    main()
