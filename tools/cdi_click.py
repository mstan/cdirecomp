#!/usr/bin/env python3
"""Move the CD-i pointer to a target screen position and click button 1.

Drives the permanent set_input surface (mask + dx/dy relative deltas) over
the debug TCP server and closes the loop against video_state's rendered
cursor position, so it works at any pace multiplier.

Usage:
  py -3 tools/cdi_click.py --port 4382 --x 320 --y 400 [--no-click]
  py -3 tools/cdi_click.py --port 4382 --where       # just report cursor
"""
import argparse
import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from cdi_debug import send  # noqa: E402

BTN1 = 0x10


def video_state(host, port):
    return send(host, port, {"cmd": "video_state"})


def set_input(host, port, mask=0, dx=None, dy=None):
    cmd = {"cmd": "set_input", "mask": mask}
    if dx is not None:
        cmd.update({"dx": dx, "dy": dy})
    return send(host, port, cmd)


def move_to(host, port, tx, ty, timeout=30.0):
    """Step the pointer toward (tx, ty), re-reading the rendered cursor."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = video_state(host, port)
        cx, cy = state["cursor_x"], state["cursor_y"]
        ex, ey = tx - cx, ty - cy
        if abs(ex) <= 1 and abs(ey) <= 1:
            set_input(host, port, 0)
            return cx, cy
        step = lambda v: max(-16, min(16, v))
        set_input(host, port, 0, dx=step(ex), dy=step(ey))
        time.sleep(0.02)
    set_input(host, port, 0)
    raise SystemExit(f"pointer did not reach ({tx},{ty}) within {timeout}s")


def click(host, port, hold=0.25):
    set_input(host, port, BTN1)
    time.sleep(hold)
    set_input(host, port, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4380)
    ap.add_argument("--x", type=int)
    ap.add_argument("--y", type=int)
    ap.add_argument("--where", action="store_true")
    ap.add_argument("--no-click", action="store_true")
    ap.add_argument("--hold", type=float, default=0.25)
    args = ap.parse_args()

    if args.where:
        state = video_state(args.host, args.port)
        print({"cursor_x": state["cursor_x"], "cursor_y": state["cursor_y"]})
        return
    if args.x is None or args.y is None:
        ap.error("--x and --y required unless --where")
    cx, cy = move_to(args.host, args.port, args.x, args.y)
    print(f"pointer at ({cx},{cy})")
    if not args.no_click:
        click(args.host, args.port, args.hold)
        print("clicked btn1")


if __name__ == "__main__":
    main()
