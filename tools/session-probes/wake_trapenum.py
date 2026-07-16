#!/usr/bin/env python3
"""TRAP-enumeration over the post-wake window: every OS-9 syscall the game
made between the 0x61E8 bit-8 wake store and the pause, with svc word,
entry regs, and return regs (carry = error)."""
import json, socket, sys

HOST, PORT = "127.0.0.1", 4382
START = int(sys.argv[1]) if len(sys.argv) > 1 else None


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
HI = cx.send({"cmd": "status"})["blocks"]
if START is None:
    stores = cx.send({"cmd": "stores", "addr": 0x61E8, "count": 64})["records"]
    START = max(0, [r for r in stores if r["val"] & 0x100][-1]["seq"] - 64)
print(f"window [{START},{HI})")


def read_word(addr):
    b = bytes.fromhex(cx.send({"cmd": "read_mem", "addr": addr, "len": 2})["bytes"].replace("--", "00"))
    return (b[0] << 8) | b[1]


SVC = {
    0x00: "F$Link", 0x01: "F$Load", 0x02: "F$UnLink", 0x0A: "F$Sleep",
    0x08: "F$Send", 0x09: "F$Icpt", 0x0B: "F$SSpd", 0x0C: "F$ID",
    0x1C: "F$SUser", 0x26: "F$Alarm", 0x2A: "F$SRqMem", 0x2B: "F$SRtMem",
    0x30: "F$Aproc", 0x82: "I$Open", 0x84: "I$Close", 0x89: "I$Read",
    0x8A: "I$ReadLn", 0x8D: "I$GetStt", 0x8E: "I$SetStt", 0x25: "F$Sigmask",
}

# collect trap entries + returns in one pass
prev = None
events = []   # (kind, seq, rec)
f = START
while f < HI:
    page = cx.send({"cmd": "trace", "from": f, "count": 256})["records"]
    if not page:
        break
    for r in page:
        pc = r["pc"]
        if pc == 0x62C and prev is not None:
            events.append(("call", r["seq"], prev["pc"], dict(r)))
        if prev is not None and prev["pc"] == 0x4076A2:
            events.append(("ret", r["seq"], pc, dict(r)))
        prev = r
    nf = page[-1]["seq"] + 1
    if nf <= f:
        break
    f = nf

opens = {}
for kind, seq, pc, r in events:
    if kind == "call":
        site = pc
        svc = read_word(site + 2)
        name = SVC.get(svc, f"svc_{svc:02X}")
        opens[site + 4] = (seq, site, svc, name, r)
        print(f"seq={seq} CALL {name:9s} (svc ${svc:02X}) site={site:#08x} "
              f"d0={r['d0']:#010x} d1={r['d1']:#010x} d2={r['d2']:#010x} "
              f"a0={r['a0']:#010x} a1={r['a1']:#010x}")
    else:
        # return lands at site+4 (or elsewhere for signals)
        if pc in opens:
            cseq, site, svc, name, cr = opens.pop(pc)
            err = "ERR" if (r["sr"] & 1) else "ok "
            print(f"seq={seq} RET  {name:9s} {err} "
                  f"d0={r['d0']:#010x} d1={r['d1']:#010x} d2={r['d2']:#010x} "
                  f"a0={r['a0']:#010x} sr={r['sr']:#06x}")
        else:
            print(f"seq={seq} RTE -> {pc:#08x} (signal/intercept dispatch) "
                  f"d0={r['d0']:#010x} d1={r['d1']:#010x} sr={r['sr']:#06x}")
