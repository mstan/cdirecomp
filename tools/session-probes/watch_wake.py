#!/usr/bin/env python3
"""Run 11: arm at attract end (lba 6900-7100); once armed, watch the FM
status block 0x61E8 for a bit-8 rising edge (game wake) and pause +50ms."""
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
prev = None
while time.monotonic() < deadline:
    if not armed:
        cs = send({"cmd": "ciap_state"})
        if 6900 < cs.get("drive_lba", 0) < 7100:
            armed = True
            w = int(send({"cmd": "read_mem", "addr": 0x61E8, "len": 2})["bytes"], 16)
            prev = bool(w & 0x100)
            print(f"armed at lba {cs['drive_lba']}, 0x61E8={w:#06x}", flush=True)
        continue
    w = int(send({"cmd": "read_mem", "addr": 0x61E8, "len": 2})["bytes"], 16)
    cur = bool(w & 0x100)
    if cur and not prev:
        time.sleep(0.05)
        p = send({"cmd": "pause"})
        print(f"paused on wake edge, 0x61E8={w:#06x}: {json.dumps(p)}", flush=True)
        print(json.dumps(send({"cmd": "status"})), flush=True)
        print(json.dumps(send({"cmd": "ciap_state"})), flush=True)
        sys.exit(0)
    prev = cur
print("timeout", flush=True)
sys.exit(1)
