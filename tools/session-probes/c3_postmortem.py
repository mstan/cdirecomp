#!/usr/bin/env python3
"""Post-mortem at the C3 pause: PCL dump + ring reconstruction of the
trigger-read op (bit3 pending values, data-handler runs, game wakes)."""
import json, socket, sys

HOST, PORT = "127.0.0.1", 4382


def send(obj):
    with socket.create_connection((HOST, PORT), timeout=10) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            c = s.recv(1 << 20)
            if not c:
                break
            buf += c
    return json.loads(buf.split(b"\n", 1)[0].decode(errors="replace"))


def read(addr, n):
    return bytes.fromhex(send({"cmd": "read_mem", "addr": addr, "len": n})["bytes"].replace("--", "00"))


def be32(b, o=0):
    return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]


A2 = 0x27E870
a1 = be32(read(A2 + 0x44, 4))
print(f"path desc a1 = {a1:#x}")
if 0 < a1 < 0x300000:
    pd = read(a1, 0x100)
    pcl = be32(pd, 0x4C)
    print(f"PCL ($4C) = {pcl:#x}  playLBA($46)={be32(pd,0x46):#x} flags69={pd[0x69]:#04x} file68={pd[0x68]:#04x}")
    if 0 < pcl < 0x300000:
        pb = read(pcl, 0x40)
        print("PCL[0:64] =", pb.hex())
        print(f"  +8 chanmask = {be32(pb,8):#010x}   +C word = {pb[0xC]<<8|pb[0xD]:#06x}")
print("pending/status/phase now:", read(A2 + 0x9C, 4).hex())

st = send({"cmd": "status"})
HI = st["blocks"]
LO = max(0, HI - 1000000)
print(f"ring scan [{LO},{HI}]")


def tr(frm, cnt):
    return send({"cmd": "trace", "from": frm, "count": cnt})["records"]


marks = {0x4294b6: "bit3(d0=pending)", 0x42871c: "data-handler", 0x429072: "game-wake",
         0x4293bc: "pcl-prep", 0x429e0c: "PCL-proc", 0x429d54: "engine-arm",
         0x4295d2: "bit3-st8-case", 0x4294fe: "bit3-st5-case", 0x42954e: "bit3-other-pend"}
f = LO
hits = []
while f < HI:
    page = tr(f, 250)
    if not page:
        break
    for r in page:
        if r["pc"] in marks:
            hits.append((r["seq"], r["pc"], r["d0"], r["d1"]))
    nf = page[-1]["seq"] + 1
    if nf <= f:
        break
    f = nf
print(f"{len(hits)} marker hits")
for seq, pc, d0, d1 in hits[-120:]:
    print(f"  seq={seq} {marks[pc]:16s} d0={d0:#010x} d1={d1:#010x}")

for addr, nm in ((0x27E90F, "pending"), (0x27E90D, "status")):
    r = send({"cmd": "stores", "addr": addr, "count": 24})
    print(f"--- {nm} stores ---")
    for rec in r["records"]:
        print(f"  seq={rec['seq']} pc={rec['pc']:#08x} val={rec['val']:#04x}")
