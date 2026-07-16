#!/usr/bin/env python3
"""Run 8: arm at attract end (lba 6900-7100), pause 300ms after the
on-target B0 reply (type-2 ikat event B000020E)."""
import json, socket, sys, time

HOST, PORT = "127.0.0.1", 4382

def send(obj):
    with socket.create_connection((HOST, PORT), timeout=5) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            c = s.recv(1 << 20)
            if not c:
                break
            buf += c
    return json.loads(buf.split(b"\n", 1)[0].decode(errors="replace"))

deadline = time.monotonic() + 300.0
armed = False
while time.monotonic() < deadline:
    if not armed:
        cs = send({"cmd": "ciap_state"})
        if 6900 < cs.get("drive_lba", 0) < 7100:
            armed = True
            print(f"armed at lba {cs['drive_lba']}", flush=True)
        continue
    r = send({"cmd": "ikat_events", "count": 6})
    if any(e.get("type") == 2 and e.get("data") == "B000020E" for e in (r.get("events") or [])):
        time.sleep(0.3)
        p = send({"cmd": "pause"})
        print(f"paused post-on-target: {json.dumps(p)}", flush=True)
        print(json.dumps(send({"cmd": "status"})), flush=True)
        print(json.dumps(send({"cmd": "ciap_state"})), flush=True)
        sys.exit(0)
print("timeout", flush=True)
sys.exit(1)
