#!/usr/bin/env python3
"""Post-mortem probes after the $DD pause: module identity around $2766xx,
PCL handle, stores-ring locate of the $DD write, and a trace window before it."""
import json, socket, sys

HOST, PORT = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 4382


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


def read(addr, ln):
    r = send({"cmd": "read_mem", "addr": addr, "len": ln})
    for k in ("bytes", "data", "hex"):
        if isinstance(r, dict) and k in r and isinstance(r[k], str):
            return bytes.fromhex(r[k])
    raise RuntimeError(f"read_mem: {r!r}")


def be16(b, o=0):
    return (b[o] << 8) | b[o + 1]


def be32(b, o=0):
    return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]


def find_module(addr):
    """Walk backward from addr on even boundaries for 4AFC sync w/ valid header parity."""
    lo = max(0, addr - 0x20000)
    chunk = read(lo, addr - lo)
    for off in range(len(chunk) - 2, -1, -2):
        if chunk[off] == 0x4A and chunk[off + 1] == 0xFC:
            base = lo + off
            hdr = chunk[off:off + 0x48] if off + 0x48 <= len(chunk) else read(base, 0x48)
            parity = 0
            for w in range(0, 0x30, 2):
                parity ^= be16(hdr, w)
            if parity != 0xFFFF:
                continue
            size = be32(hdr, 0x08)
            name_off = be32(hdr, 0x0C)
            if base + size < addr:  # module ends before target; keep walking
                continue
            name = read(base + name_off, 32).split(b"\0")[0].decode("ascii", "replace")
            return {"base": hex(base), "size": hex(size), "name": name,
                    "type_lang": hex(be16(hdr, 0x12)), "target_off": hex(addr - base)}
    return None


print("== registers ==")
print(json.dumps(send({"cmd": "get_registers"})))

print("== module owning $276608 ==")
print(json.dumps(find_module(0x276608)))

print("== PCL handle *(0x4B52) ==")
pcl = be32(read(0x4B52, 4))
print(f"PCL = {pcl:#010x}")
if 0 < pcl < 0x300000:
    head = read(pcl, 32)
    print("PCL[0:32] =", head.hex())
    print(f"*(PCL) path number (word) = {be16(head):#06x}  (long={be32(head):#010x})")

print("== stores to 0x400C (last 8) ==")
st = send({"cmd": "stores", "addr": 0x400C, "count": 8})
print(json.dumps(st))

recs = st.get("records", [])
dd = [r for r in recs if (r["val"] & 0xFF) == 0xDD]
if dd:
    hit = dd[-1]
    seq = hit["seq"]
    print(f"== $DD store: pc={hit['pc']:#x} seq={seq} ==")
    tr = send({"cmd": "trace", "from": max(0, seq - 48), "count": 64})
    print(f"trace total={tr.get('total')}")
    for r in tr.get("records", []):
        print(f"  seq={r['seq']} pc={r['pc']:#08x} op={r['op']:#06x} "
              f"d0={r['d0']:#010x} d1={r['d1']:#010x} a0={r['a0']:#010x} a7={r['a7']:#010x} sr={r['sr']:#06x}")
else:
    print("no $DD store found in the last 8 covering stores")
