#!/usr/bin/env python3
"""cdi_debug.py — line client for the CdiRuntime TCP debug server (TCP.md).

The debug server is the sanctioned way to inspect a run; this is the reference
client the other recomp projects each ship. It speaks the JSON-line protocol:
one command object per line, one response line back.

Examples:
    python tools/cdi_debug.py ping
    python tools/cdi_debug.py status
    python tools/cdi_debug.py get_registers
    python tools/cdi_debug.py read_mem --addr 0x400000 --len 32
    python tools/cdi_debug.py trace --count 32
    python tools/cdi_debug.py dispatch_miss_info
    python tools/cdi_debug.py --port 4381 status      # query the oracle

The runtime answers on 127.0.0.1:4380 (native) / 4381 (oracle, +1).
"""
import argparse, json, socket, struct, sys, time


def send(host, port, obj):
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    line = buf.split(b"\n", 1)[0].decode(errors="replace")
    return json.loads(line) if line.strip().startswith("{") else line


def screenshot(host, port, path):
    """Freeze one already-running observer and save its published ARGB frame."""
    paused = send(host, port, {"cmd": "pause"})
    if not isinstance(paused, dict) or not paused.get("ok"):
        raise RuntimeError(f"pause failed: {paused!r}")
    seq = int(paused.get("seq", 0))
    # The CPU can consume the pause flag before the server finishes writing
    # the acknowledgement.  In that race the acknowledged seq is already the
    # parked seq, so requiring blocks > seq rejects a correctly frozen
    # observer.  Require a post-request sequence at or beyond the acknowledged
    # value to remain stable instead; a merely pending request will either
    # advance to its final boundary or fail the bounded timeout.
    deadline = time.monotonic() + 5.0
    stable_blocks = None
    stable_since = None
    while time.monotonic() < deadline:
        state = send(host, port, {"cmd": "status"})
        blocks = int(state.get("blocks", 0))
        now = time.monotonic()
        if blocks >= seq:
            if blocks != stable_blocks:
                stable_blocks = blocks
                stable_since = now
            elif stable_since is not None and now - stable_since >= 0.1:
                break
        else:
            stable_blocks = None
            stable_since = None
        time.sleep(0.01)
    else:
        raise RuntimeError("observer did not park after the immediate pause request")

    info = send(host, port, {"cmd": "video_frame"})
    if not isinstance(info, dict) or not info.get("ok"):
        raise RuntimeError(f"video_frame failed: {info!r}")
    width, height = int(info["width"]), int(info["height"])
    rgb = bytearray(width * height * 3)
    pos = 0
    generation = None
    for y in range(height):
        row = send(host, port, {"cmd": "video_scanline", "y": y})
        if (not isinstance(row, dict) or not row.get("ok") or
                int(row.get("width", -1)) != width or
                int(row.get("height", -1)) != height):
            raise RuntimeError(f"video_scanline {y} failed: {row!r}")
        row_generation = int(row["generation"])
        if generation is None:
            generation = row_generation
        elif row_generation != generation:
            raise RuntimeError("published frame changed after observer was parked")
        argb = bytes.fromhex(row["argb"])
        if len(argb) != width * 4:
            raise RuntimeError(f"video_scanline {y} returned {len(argb)} bytes, expected {width * 4}")
        for x in range(width):
            src = x * 4
            rgb[pos:pos + 3] = argb[src + 1:src + 4]
            pos += 3
    with open(path, "wb") as output:
        if path.lower().endswith(".bmp"):
            row_bytes = (width * 3 + 3) & ~3
            pixel_bytes = row_bytes * height
            output.write(b"BM")
            output.write(struct.pack("<IHHI", 54 + pixel_bytes, 0, 0, 54))
            output.write(struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24,
                                     0, pixel_bytes, 2835, 2835, 0, 0))
            padding = b"\0" * (row_bytes - width * 3)
            for y in range(height - 1, -1, -1):
                start = y * width * 3
                row = rgb[start:start + width * 3]
                bgr = bytearray(len(row))
                for x in range(width):
                    src = x * 3
                    bgr[src:src + 3] = row[src:src + 3][::-1]
                output.write(bgr)
                output.write(padding)
        else:
            output.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
            output.write(rgb)
    return {"ok": True, "path": path, "width": width, "height": height,
            "generation": generation, "argb_fnv1a": info.get("argb_fnv1a")}


def main():
    ap = argparse.ArgumentParser(description="CdiRuntime debug client")
    ap.add_argument(
        "cmd",
        help="ping|status|pause|video_frame|frame_hashes|video_scanline|screenshot|video_state|disc_state|mount_disc|"
             "eject_disc|get_registers|read_mem|trace|cycle_trace|stores|"
             "set_input|emu_ikat_state|ikat_events|ciap_events|irq_events|"
             "interp_report|indirect_targets|dispatch_miss_info|quit",
    )
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4380)
    ap.add_argument("--addr", type=lambda x: int(x, 0), help="read_mem address")
    ap.add_argument("--len", type=lambda x: int(x, 0), help="read_mem length")
    ap.add_argument("--y", type=lambda x: int(x, 0), help="video_scanline row")
    ap.add_argument("--count", type=lambda x: int(x, 0), help="trace record count")
    ap.add_argument("--mask", type=lambda x: int(x, 0), help="set_input CDI_INPUT_* bitmask")
    ap.add_argument("--path", help="mount_disc input or screenshot BMP/PPM output path")
    ap.add_argument("--from", dest="frm", type=lambda x: int(x, 0),
                    help="trace seq/frame_hashes frame start (forward paging); omit for the most-recent tail")
    args = ap.parse_args()

    if args.cmd == "screenshot" and not args.path:
        ap.error("screenshot requires --path <output.ppm>")

    req = {"cmd": args.cmd}
    if args.addr is not None:
        req["addr"] = args.addr
    if args.len is not None:
        req["len"] = args.len
    if args.y is not None:
        req["y"] = args.y
    if args.count is not None:
        req["count"] = args.count
    if args.mask is not None:
        req["mask"] = args.mask
    if args.path is not None:
        req["path"] = args.path
    if args.frm is not None:
        req["from"] = args.frm

    try:
        resp = (screenshot(args.host, args.port, args.path)
                if args.cmd == "screenshot" else
                send(args.host, args.port, req))
    except RuntimeError as e:
        print(f"screenshot failed: {e}", file=sys.stderr)
        return 1
    except (ConnectionRefusedError, OSError) as e:
        print(f"cannot reach debug server at {args.host}:{args.port} ({e})", file=sys.stderr)
        print("  is CdiRuntime running? (start it with --hold to keep it queryable)", file=sys.stderr)
        return 2

    print(json.dumps(resp, indent=2) if isinstance(resp, dict) else resp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
