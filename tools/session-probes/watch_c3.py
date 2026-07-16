#!/usr/bin/env python3
"""Run 7 watcher: arm when the attract stream nears its end (lba 6900-7100),
then pause the instant the driver sends IKAT C3 (stop/give-up)."""
import json, socket, sys, time

HOST, PORT = "127.0.0.1", 4382
TIMEOUT = 300.0


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


deadline = time.monotonic() + TIMEOUT
armed = False
while time.monotonic() < deadline:
    if not armed:
        cs = send({"cmd": "ciap_state"})
        if 6900 < cs.get("drive_lba", 0) < 7100:
            armed = True
            print(f"armed at lba {cs['drive_lba']}", flush=True)
        continue
    r = send({"cmd": "ikat_events", "count": 6})
    evs = r.get("events") or []
    if any(e.get("type") == 1 and e.get("data") == "C3000000" for e in evs):
        p = send({"cmd": "pause"})
        print(f"paused on C3: {json.dumps(p)}", flush=True)
        print(json.dumps(send({"cmd": "status"})), flush=True)
        print(json.dumps(send({"cmd": "ciap_state"})), flush=True)
        sys.exit(0)
print("timeout", flush=True)
sys.exit(1)
