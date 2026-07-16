#!/usr/bin/env python3
"""Deterministic anchor at cdfm's SS_Play wait-exit branch ($41537E): arm a
stop_pc there once the scene-swap setup dip (.map re-read, lba<6000) is seen,
then wait for the halt. skip=N (argv[1]) selects the (N+1)-th hit."""
import json, socket, sys, time

HOST, PORT = "127.0.0.1", 4382
SKIP = int(sys.argv[1]) if len(sys.argv) > 1 else 0
STOP_PC = 0x41537E


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
while time.monotonic() < deadline:
    if phase <= 2:
        lba = send({"cmd": "ciap_state"}).get("drive_lba", 0)
        if phase == 1 and lba > 6900:
            phase = 2
            print(f"phase2 armed (intro tail, lba={lba})", flush=True)
        elif phase == 2 and 0 < lba < 6000:
            r = send({"cmd": "stop_pc", "pc": STOP_PC, "skip": SKIP})
            print(f"phase3: stop_pc armed at dip (lba={lba}): {json.dumps(r)}", flush=True)
            phase = 3
        continue
    st = send({"cmd": "status"})
    if st.get("halted"):
        print(f"HALTED at pc={st['pc']:#x} seq={st['blocks']}", flush=True)
        print(json.dumps(st), flush=True)
        print(json.dumps(send({"cmd": "ciap_state"})), flush=True)
        sys.exit(0)
    time.sleep(0.02)
print(f"timeout in phase {phase}", flush=True)
sys.exit(1)
