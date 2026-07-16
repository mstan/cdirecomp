#!/usr/bin/env python3
"""Non-freezing screenshot: assemble the live frame from video_scanline rows.

Unlike cdi_debug.py's `screenshot` (which pauses the CPU thread permanently
via the fault-hold park — post-mortem use only), this never pauses: it pulls
each row of the published frame over TCP and writes a row-doubled PNG, so it
is safe inside a look -> input -> verify navigation loop.

Usage: py -3 tools/cdi_view.py --port 4382 --path build/tmp/live.png
"""
import argparse
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from cdi_debug import send  # noqa: E402


def capture(host, port, path):
    first = send(host, port, {"cmd": "video_scanline", "y": 0})
    if not first.get("ok"):
        raise SystemExit(f"video_scanline failed: {first!r}")
    width, height = first["width"], first["height"]
    rows = []
    for y in range(height):
        r = first if y == 0 else send(host, port,
                                      {"cmd": "video_scanline", "y": y})
        argb = r["argb"]
        px = b"".join(int(argb[i + 2:i + 8], 16).to_bytes(3, "big")
                      for i in range(0, len(argb), 8))
        rows.append(b"\x00" + px)
    raw = b"".join(r + r for r in rows)  # double rows for display aspect

    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d +
                struct.pack(">I", zlib.crc32(t + d)))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height * 2,
                                      8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 6)) +
           chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)
    return width, height, first["generation"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4380)
    ap.add_argument("--path", required=True)
    args = ap.parse_args()
    w, h, gen = capture(args.host, args.port, args.path)
    print(f"{args.path}: {w}x{h} generation={gen}")


if __name__ == "__main__":
    main()
