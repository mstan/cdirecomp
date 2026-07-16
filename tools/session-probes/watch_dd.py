#!/usr/bin/env python3
"""Poll 0x400C (FM last-error slot) on the debug server; pause the instant it
reads 0xDD so the 1M-block trace ring still holds the failing TRAP window."""
import json, socket, sys, time

HOST, PORT = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 4382
TIMEOUT = float(sys.argv[2]) if len(sys.argv) > 2 else 180.0


def send(obj):
    with socket.create_connection((HOST, PORT), timeout=5) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    line = buf.split(b"\n", 1)[0].decode(errors="replace")
    return json.loads(line)


def read_long(addr):
    r = send({"cmd": "read_mem", "addr": addr, "len": 4})
    return int(r["bytes"], 16)


deadline = time.monotonic() + TIMEOUT
val = read_long(0x400C)
print(f"initial 0x400C.l = {val:#010x}", flush=True)
n = 0
while time.monotonic() < deadline:
    val = read_long(0x400C)
    n += 1
    if (val & 0xFF) == 0xDD:
        time.sleep(0.4)  # let the F$Load fallback + park entry land in the ring
        p = send({"cmd": "pause"})
        st = send({"cmd": "status"})
        print(f"HIT after {n} polls: 0x400C.l={val:#010x}; pause={json.dumps(p)}")
        print(f"status={json.dumps(st)}")
        sys.exit(0)
print(f"timeout after {n} polls, last 0x400C.l={val:#010x}")
sys.exit(1)
