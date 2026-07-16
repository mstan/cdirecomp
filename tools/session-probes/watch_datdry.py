#!/usr/bin/env python3
"""Run 15: log every transition of the DATA consumer table (0x4C16..0x4C95)
from game start onward WITHOUT pausing; once the table has been nonzero and
then stays all-zero for 200ms, pause — the clearing/consuming event is then
~260k blocks back, well inside the 1M trace ring."""
import json, socket, sys, time

HOST, PORT = "127.0.0.1", 4382
DAT, VID = 0x4C16, 0x4B56


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


def table(base):
    b = bytes.fromhex(send({"cmd": "read_mem", "addr": base, "len": 128})["bytes"].replace("--", "00"))
    return {ch: int.from_bytes(b[ch * 4:ch * 4 + 4], "big") for ch in range(32)
            if int.from_bytes(b[ch * 4:ch * 4 + 4], "big")}


deadline = time.monotonic() + 300.0
prev_d, prev_v = None, None
ever_nonzero = False
zero_since = None
armed = False
while time.monotonic() < deadline:
    if not armed:
        cs = send({"cmd": "ciap_state"})
        if cs.get("drive_lba", 0) > 3200:
            armed = True
            print(f"armed at lba {cs['drive_lba']}", flush=True)
        continue
    d = {ch: p for ch, p in table(DAT).items() if 0x4000 <= p < 0x280000}
    v = {ch: p for ch, p in table(VID).items() if 0x4000 <= p < 0x280000}
    now = time.monotonic()
    if d != prev_d or v != prev_v:
        st = send({"cmd": "status"})
        print(f"blocks={st['blocks']} DAT={{{','.join(f'{k}:{x:#x}' for k,x in d.items())}}} "
              f"VID={{{','.join(f'{k}:{x:#x}' for k,x in v.items())}}}", flush=True)
        prev_d, prev_v = d, v
    if d:
        ever_nonzero = True
        zero_since = None
    elif ever_nonzero:
        if zero_since is None:
            zero_since = now
        elif now - zero_since > 0.2:
            p = send({"cmd": "pause"})
            print(f"paused after DAT dry 200ms: {json.dumps(p)}", flush=True)
            print(json.dumps(send({"cmd": "status"})), flush=True)
            sys.exit(0)
print("timeout", flush=True)
sys.exit(1)
