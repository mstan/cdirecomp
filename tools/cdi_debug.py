#!/usr/bin/env python3
"""cdi_debug.py — line client for the CdiRuntime TCP debug server (TCP.md).

The debug server is the sanctioned way to inspect a run; this is the reference
client the other recomp projects each ship. It speaks the JSON-line protocol:
one command object per line, one response line back.

Examples:
    python tools/cdi_debug.py ping
    python tools/cdi_debug.py status
    python tools/cdi_debug.py get_registers
    python tools/cdi_debug.py read_mem --addr 0x400000 --len 32
    python tools/cdi_debug.py trace --count 32
    python tools/cdi_debug.py dispatch_miss_info
    python tools/cdi_debug.py --port 4381 status      # query the oracle

The runtime answers on 127.0.0.1:4380 (native) / 4381 (oracle, +1).
"""
import argparse, json, socket, sys


def send(host, port, obj):
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    line = buf.split(b"\n", 1)[0].decode(errors="replace")
    return json.loads(line) if line.strip().startswith("{") else line


def main():
    ap = argparse.ArgumentParser(description="CdiRuntime debug client")
    ap.add_argument("cmd", help="ping|status|get_registers|read_mem|trace|dispatch_miss_info|quit")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4380)
    ap.add_argument("--addr", type=lambda x: int(x, 0), help="read_mem address")
    ap.add_argument("--len", type=lambda x: int(x, 0), help="read_mem length")
    ap.add_argument("--count", type=lambda x: int(x, 0), help="trace record count")
    args = ap.parse_args()

    req = {"cmd": args.cmd}
    if args.addr is not None:
        req["addr"] = args.addr
    if args.len is not None:
        req["len"] = args.len
    if args.count is not None:
        req["count"] = args.count

    try:
        resp = send(args.host, args.port, req)
    except (ConnectionRefusedError, OSError) as e:
        print(f"cannot reach debug server at {args.host}:{args.port} ({e})", file=sys.stderr)
        print("  is CdiRuntime running? (start it with --hold to keep it queryable)", file=sys.stderr)
        return 2

    print(json.dumps(resp, indent=2) if isinstance(resp, dict) else resp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
