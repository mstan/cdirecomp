#!/usr/bin/env python3
"""Run 13: pause ~60ms after the scene-2 video-ch15 consumer slot (0x4B92)
first becomes nonzero — the ring then holds the whole registration flow."""
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
while time.monotonic() < deadline:
    v = int(send({"cmd": "read_mem", "addr": 0x4B92, "len": 4})["bytes"].replace("--", "00"), 16)
    if v:
        time.sleep(0.06)
        p = send({"cmd": "pause"})
        print(f"paused: 0x4B92={v:#010x} {json.dumps(p)}", flush=True)
        print(json.dumps(send({"cmd": "status"})), flush=True)
        sys.exit(0)
print("timeout", flush=True)
sys.exit(1)
