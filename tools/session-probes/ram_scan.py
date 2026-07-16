#!/usr/bin/env python3
"""Scan guest RAM over the debug TCP for a byte string; print hits."""
import json, socket, sys

HOST, PORT = "127.0.0.1", 4382
NEEDLE = sys.argv[1].encode() if len(sys.argv) > 1 else b"cdi_intro_sub"
LO = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x0
HI = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x280000


def send(obj):
    with socket.create_connection((HOST, PORT), timeout=10) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.split(b"\n", 1)[0].decode(errors="replace"))


CHUNK = 0x1000
overlap = len(NEEDLE) - 1
prev_tail = b""
addr = LO
hits = []
while addr < HI:
    n = min(CHUNK, HI - addr)
    r = send({"cmd": "read_mem", "addr": addr, "len": n})
    data = bytes.fromhex(r["bytes"].replace("--", "00"))
    blob = prev_tail + data
    off = 0
    while True:
        i = blob.find(NEEDLE, off)
        if i < 0:
            break
        hits.append(addr - len(prev_tail) + i)
        off = i + 1
    prev_tail = data[-overlap:] if overlap else b""
    addr += n
for h in hits:
    print(hex(h))
print(f"{len(hits)} hits for {NEEDLE!r} in [{LO:#x},{HI:#x})")
