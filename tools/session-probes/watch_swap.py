#!/usr/bin/env python3
"""Anchor on the scene-swap setup: intro tail (lba>6900) -> .map re-read dip
(lba<6000) -> trigger-read return (lba>6900), then pause ~50ms after the first
0x61E8 bit8|bit15 rising edge — the wake that releases cdfm's SS_Play wait
loop ($41540C), so the ring holds the $41537E/$4153AC branch decision."""
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
phase = 1
prev = None
while time.monotonic() < deadline:
    if phase <= 3:
        lba = send({"cmd": "ciap_state"}).get("drive_lba", 0)
        if phase == 1 and lba > 6900:
            phase = 2
            print(f"phase2 armed (intro tail, lba={lba})", flush=True)
        elif phase == 2 and 0 < lba < 6000:
            phase = 3
            print(f"phase3 armed (setup dip, lba={lba})", flush=True)
        elif phase == 3 and lba > 6900:
            phase = 4
            w = int(send({"cmd": "read_mem", "addr": 0x61E8, "len": 2})["bytes"], 16)
            prev = bool(w & 0x8100)
            print(f"phase4 armed (trigger read, lba={lba}, 0x61E8={w:#06x})", flush=True)
        continue
    w = int(send({"cmd": "read_mem", "addr": 0x61E8, "len": 2})["bytes"], 16)
    cur = bool(w & 0x8100)
    if cur and not prev:
        time.sleep(0.05)
        p = send({"cmd": "pause"})
        print(f"paused on completion edge, 0x61E8={w:#06x}: {json.dumps(p)}", flush=True)
        print(json.dumps(send({"cmd": "status"})), flush=True)
        print(json.dumps(send({"cmd": "ciap_state"})), flush=True)
        sys.exit(0)
    prev = cur
print(f"timeout in phase {phase}", flush=True)
sys.exit(1)
