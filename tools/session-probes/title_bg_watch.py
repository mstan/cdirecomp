#!/usr/bin/env python3
"""Watch the Hotel Mario title screen: log seeks, drive state, and screen changes.

Usage: py -3 tools/session-probes/title_bg_watch.py --port 4382 --seconds 120
Saves a PNG on every visual change to build/tmp/titlebg-<n>.png and prints a
timestamped ledger of IKAT commands (E0/E1/C3/C2) + ciap drive state.
"""
import argparse
import hashlib
import sys
import time

sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 2)[0])
from cdi_debug import send  # noqa: E402
from cdi_view import capture  # noqa: E402


def bcd_lba(h):
    b = bytes.fromhex(h)
    m = (b[1] >> 4) * 10 + (b[1] & 15)
    s = (b[2] >> 4) * 10 + (b[2] & 15)
    f = (b[3] >> 4) * 10 + (b[3] & 15)
    return (m * 60 + s) * 75 + f - 150


def frame_hash(host, port):
    rows = []
    for y in range(0, 240, 8):
        r = send(host, port, {"cmd": "video_scanline", "y": y})
        rows.append(r["argb"])
    return hashlib.sha1("".join(rows).encode()).hexdigest()[:12]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4382)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--seconds", type=float, default=120.0)
    a = ap.parse_args()

    last_seq = None
    last_hash = None
    shot = 0
    t0 = time.monotonic()
    while time.monotonic() - t0 < a.seconds:
        el = time.monotonic() - t0
        st = send(a.host, a.port, {"cmd": "status"})
        cs = send(a.host, a.port, {"cmd": "ciap_state"})
        ik = send(a.host, a.port, {"cmd": "ikat_events", "count": 60})
        evs = ik.get("events", [])
        for e in evs:
            if last_seq is not None and e["seq"] <= last_seq:
                continue
            if e["type"] == 1:
                c = e["data"][:2]
                extra = ""
                if c in ("E0", "E1"):
                    extra = " lba=%d" % bcd_lba(e["data"])
                print("%7.1fs frame=%d CMD %s%s" % (el, e["frame"], c, extra),
                      flush=True)
        if evs:
            last_seq = max(e["seq"] for e in evs)
        h = frame_hash(a.host, a.port)
        if h != last_hash:
            shot += 1
            path = "build/tmp/titlebg-%02d.png" % shot
            capture(a.host, a.port, path)
            print("%7.1fs frame=%d SCREEN-CHANGE hash=%s -> %s"
                  % (el, st.get("frame", -1), h, path), flush=True)
            last_hash = h
        print("%7.1fs frame=%d lba=%d ack=%d run=%d pc=%x"
              % (el, st.get("frame", -1), cs["drive_lba"],
                 cs["waiting_ack"], cs["running"], st.get("pc", 0)),
              flush=True)
        time.sleep(2.0)


if __name__ == "__main__":
    main()
