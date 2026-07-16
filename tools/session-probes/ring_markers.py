#!/usr/bin/env python3
"""Full-ring marker sweep: scan the entire live trace ring for the game's
$060A-handshake machinery (checker/resubmit/watch veneers/cdfm gate) plus
wake-handler entries, printing each hit with registers."""
import json, socket, sys

HOST, PORT = "127.0.0.1", 4382


class Conn:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=15)
        self.buf = b""

    def send(self, obj):
        self.s.sendall((json.dumps(obj) + "\n").encode())
        while b"\n" not in self.buf:
            c = self.s.recv(1 << 20)
            if not c:
                raise RuntimeError("conn closed")
            self.buf += c
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode(errors="replace"))


MARKS = {
    0x260474: "checker($60A poll)",
    0x260562: "resubmit",
    0x2604BA: "wake-handler",
    0x2604FC: "arm-wait($402B=1)",
    0x2601D8: "checker-err-path",
    0x276614: "veneer$31",
    0x27663C: "veneer$34",
    0x276670: "veneer$36",
    0x276674: "watch-TRAP",
    0x415778: "cdfm-watch-gate",
    0x4283DC: "drv-SetStat$31",
    0x42846A: "drv-SetStat$34",
    0x4284FE: "drv-SetStat$36",
    0x429072: "drv-game-wake",
}

cx = Conn()
HI = cx.send({"cmd": "status"})["blocks"]
LO = max(0, HI - (1 << 20))
print(f"scanning ring [{LO},{HI})")
f = LO
hits = 0
while f < HI:
    page = cx.send({"cmd": "trace", "from": f, "count": 256})["records"]
    if not page:
        break
    for r in page:
        if r["pc"] in MARKS:
            print(f"seq={r['seq']} {MARKS[r['pc']]:20s} pc={r['pc']:#08x} "
                  f"d0={r['d0']:#010x} d1={r['d1']:#010x} d2={r['d2']:#010x} "
                  f"a0={r['a0']:#010x} a1={r['a1']:#010x} sr={r['sr']:#06x}")
            hits += 1
    nf = page[-1]["seq"] + 1
    if nf <= f:
        break
    f = nf
print(f"done: {hits} hits over [{LO},{f})")
