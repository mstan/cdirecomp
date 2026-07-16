#!/usr/bin/env python3
"""Post-mortem at the wake-edge pause (run 12+): locate the 0x61E8 bit-8
rising store, then walk the trace ring forward from it and print the
control-flow edges (discontinuous PC transitions), so the game's callback
state machine can be followed from the driver wake ($429072) to wherever
it parks without issuing the watch SetStat ($31/$34/$36 -> $060A)."""
import json, socket, sys

HOST, PORT = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 4382


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


cx = Conn()
st = cx.send({"cmd": "status"})
HI = st["blocks"]
print(f"total seq = {HI}")

# 1. Find the wake store: last store to 0x61E8 that set bit8 (word write with
#    0x100, or byte write of odd value to 0x61E8 itself).
stores = cx.send({"cmd": "stores", "addr": 0x61E8, "count": 64})["records"]
print(f"--- last {len(stores)} stores covering 0x61E8 ---")
for r in stores:
    print(f"  seq={r['seq']} pc={r['pc']:#08x} addr={r['addr']:#06x} val={r['val']:#06x} sz={r['size']}")

wake = None
for r in stores:
    v, a, sz = r["val"], r["addr"], r["size"]
    bit8 = (v & 0x100) if (a == 0x61E8 and sz >= 2) else ((v & 0x01) if (a == 0x61E8 and sz == 1) else 0)
    if bit8:
        wake = r
print(f"wake store: {wake}")
if not wake:
    sys.exit("no bit8-setting store found in window")

# 0x61EA stores too (the $060A slot) for reference
st2 = cx.send({"cmd": "stores", "addr": 0x61EA, "count": 16})["records"]
print("--- last stores covering 0x61EA ---")
for r in st2:
    print(f"  seq={r['seq']} pc={r['pc']:#08x} addr={r['addr']:#06x} val={r['val']:#06x} sz={r['size']}")

# 2. Walk trace forward from just before the wake store; print flow edges.
START = max(0, wake["seq"] - 64)
prev_pc = None
edges = 0
f = START
out = open("build/tmp/wake_flow.txt", "w")
while f < HI:
    resp = cx.send({"cmd": "trace", "from": f, "count": 256})
    page = resp["records"]
    if not page:
        break
    for r in page:
        pc = r["pc"]
        if prev_pc is None or not (0 < pc - prev_pc <= 10):
            # control-flow edge (branch/jump/call/rts or trace gap)
            line = (f"seq={r['seq']} {prev_pc if prev_pc is None else f'{prev_pc:#08x}'}"
                    f" -> {pc:#08x} sr={r['sr']:#06x}"
                    f" d0={r['d0']:#010x} d1={r['d1']:#010x} d2={r['d2']:#010x}"
                    f" a0={r['a0']:#010x} a1={r['a1']:#010x} a2={r['a2']:#010x}"
                    f" a6={r['a6']:#010x} a7={r['a7']:#010x}")
            out.write(line + "\n")
            edges += 1
        prev_pc = pc
    nf = page[-1]["seq"] + 1
    if nf <= f:
        break
    f = nf
out.close()
print(f"walked [{START},{f}) : {edges} flow edges -> build/tmp/wake_flow.txt")
