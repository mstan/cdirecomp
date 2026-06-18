#!/usr/bin/env python3
"""check_dispatch_misses.py — RULE 0a enforcement (DEBUG.md).

A dispatch miss means call_by_address found no generated function at a target:
an entire subroutine was skipped — a silent, game-breaking bug. This queries a
running CdiRuntime's debug server and exits non-zero if any miss was recorded,
so it can gate a boot smoke run in CI or a pre-commit check.

Usage:
    python tools/check_dispatch_misses.py [--port 4380]

Exit codes: 0 = no misses, 1 = misses recorded, 2 = server unreachable.
"""
import argparse, json, socket, sys


def query(host, port, cmd):
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall((json.dumps({"cmd": cmd}) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.split(b"\n", 1)[0].decode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4380)
    args = ap.parse_args()

    try:
        info = query(args.host, args.port, "dispatch_miss_info")
    except (ConnectionRefusedError, OSError) as e:
        print(f"unreachable: {args.host}:{args.port} ({e})", file=sys.stderr)
        return 2

    n = info.get("count", 0)
    if n == 0:
        print("dispatch misses: 0 — clean (RULE 0a satisfied)")
        return 0
    uniq = info.get("unique", [])
    print(f"dispatch misses: {n} ({len(uniq)} unique) — RULE 0a VIOLATED")
    for a in uniq:
        print(f"  $%08X" % a)
    print("resolve these (discover the function / fix the finder/loader) before "
          "any other debugging.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
