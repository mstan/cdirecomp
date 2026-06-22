#!/usr/bin/env python3
"""interp_report.py — classify the hybrid interpreter's fallback (always-on).

The recompiler covers what it could statically discover; everything else runs on
the clean-room interpreter (m68k_interp.c). "The interpreter ran X% of boot" is
useless on its own — this asks the runtime's always-on fallback aggregate WHICH
PCs ran interpreted, WHY they fell through, and in WHICH memory region, so we can
tell whether the interpreter is a safety net or quietly becoming the runtime
(and which RAM regions are content-hash-promotion candidates).

Reads the `interp_report` TCP command (TCP.md). Examples:
    python tools/interp_report.py                       # top 40, native :4380
    python tools/interp_report.py --port 4396 --count 60
    python tools/interp_report.py --reason dispatch_miss # only PCs that missed
    python tools/interp_report.py --reset                # zero counters after reading

Nothing here arms anything — the aggregate records continuously from boot; this
just queries it (per the always-on-ring discipline in DEBUG.md).
"""
import argparse, json, socket, sys

# Index order must match the FB_* / FB_REG_* enums in runner/include/debug_server.h
# and runner/src/debug_server.c respectively.
REASONS = ["none", "dispatch_miss", "top_resume", "bus_handler", "exception"]
REGIONS = ["vectors", "ram0", "ram1", "rom", "other"]


def send(host, port, obj):
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.split(b"\n", 1)[0].decode(errors="replace"))


def reasons_from_mask(mask):
    return "|".join(REASONS[i] if i < len(REASONS) else f"r{i}"
                    for i in range(16) if mask & (1 << i)) or "-"


def pct(n, d):
    return f"{100.0 * n / d:5.1f}%" if d else "  0.0%"


def main():
    ap = argparse.ArgumentParser(description="CdiRuntime interpreter-fallback classifier")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4380)
    ap.add_argument("--count", type=lambda x: int(x, 0), default=40, help="top PCs to list")
    ap.add_argument("--reason", choices=REASONS, help="restrict top list to PCs that saw this reason")
    ap.add_argument("--reset", action="store_true", help="zero all counters after reading")
    ap.add_argument("--json", action="store_true", help="dump raw JSON instead of a table")
    args = ap.parse_args()

    req = {"cmd": "interp_report", "count": args.count}
    if args.reason is not None:
        req["reason"] = REASONS.index(args.reason)
    if args.reset:
        req["reset"] = 1

    try:
        r = send(args.host, args.port, req)
    except (ConnectionRefusedError, OSError) as e:
        print(f"cannot reach debug server at {args.host}:{args.port} ({e})", file=sys.stderr)
        print("  is CdiRuntime running? (start it with --hold or --stop-seq to keep it queryable)",
              file=sys.stderr)
        return 2

    if not r.get("ok"):
        print(json.dumps(r, indent=2)); return 1
    if args.json:
        print(json.dumps(r, indent=2)); return 0

    interp = r["total"]
    native = r["native"]
    exec_total = interp + native
    print("== interpreter-fallback report ==")
    print(f"  interpreted instructions : {interp:>12,}  ({pct(interp, exec_total)} of executed)")
    print(f"  recompiled (native)      : {native:>12,}  ({pct(native, exec_total)} of executed)")
    print(f"  distinct interpreted PCs : {r['distinct']:>12,}")
    if r.get("dropped"):
        print(f"  DROPPED (hash full)      : {r['dropped']:>12,}  (raise CDI_FB_HASH_LEN)")

    print("\n  by reason:")
    for i, c in enumerate(r["by_reason"]):
        label = REASONS[i] if i < len(REASONS) else f"r{i}"
        print(f"    {label:<14} {c:>12,}  {pct(c, interp)}")
    print("  by region:")
    for i, c in enumerate(r["by_region"]):
        label = REGIONS[i] if i < len(REGIONS) else f"reg{i}"
        print(f"    {label:<14} {c:>12,}  {pct(c, interp)}")

    top = r.get("top", [])
    hdr = f"\n  top {len(top)} interpreted PCs"
    if args.reason:
        hdr += f" (reason={args.reason})"
    print(hdr + ":")
    print(f"    {'PC':<10} {'count':>12} {'%interp':>8}  {'region':<8} reasons")
    for e in top:
        print(f"    ${e['pc']:06X}   {e['count']:>12,} {pct(e['count'], interp):>8}  "
              f"{REGIONS[e['region']]:<8} {reasons_from_mask(e['reasons'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
