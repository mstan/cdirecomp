#!/usr/bin/env python3
"""trace_pcs.py — dump compact PC/SR list from a trace window (debug aid)."""
import argparse, json, socket


def query(port, obj):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            c = s.recv(65536)
            if not c:
                break
            buf += c
    return json.loads(buf.split(b"\n", 1)[0].decode())


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4390)
    ap.add_argument("--from", dest="frm", type=int, required=True)
    ap.add_argument("--count", type=int, default=40)
    a = ap.parse_args()
    recs = query(a.port, {"cmd": "trace", "from": a.frm, "count": a.count}).get("records", [])
    for r in recs:
        print(f"seq {r['seq']:>7} PC=${r['pc']:06X} SR=${r['sr']:04X}")
