#!/usr/bin/env python3
"""collect_seeds.py — trace-guided discovery: union the runtime's observed
uncovered control-flow entries into the recompiler's seed file.

The static function finder cannot follow register-indirect `JSR (An)` through
dispatch tables the OS-9 kernel builds at runtime, so those functions surface as
dispatch misses and run interpreted (see tools/interp_report.py). The runtime
records missed indirect targets (`indirect_targets` TCP command); this unions
the in-ROM ones into bios/cdrtos_discovered.txt, which CdiRecompBios re-seeds.
Every instruction already emitted in a canonical body has an async native
resume row, so exception resumes only appear here when they expose a genuinely
undiscovered CFG. Run after a new BIOS path, regenerate, and repeat until no NEW
targets appear — the general coverage loop, not a hand-picked patch.

    python tools/collect_seeds.py --port 4396          # union into the default file
    python tools/collect_seeds.py --port 4396 --dry-run # show new targets, don't write

Exit status 0 always; the "new targets" count tells the loop whether to iterate.
"""
import argparse, json, os, socket, sys

ROM_LO = 0x400000
ROM_HI = 0x500000          # CdiRecompBios applies the exact img_size bound; keep this lenient
DEFAULT_FILE = os.path.join("bios", "cdrtos_discovered.txt")


def query(host, port, obj):
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.split(b"\n", 1)[0].decode(errors="replace"))


def read_existing(path):
    seen = set()
    if not os.path.exists(path):
        return seen
    with open(path, encoding="utf-8") as f:
        for line in f:
            p = line.strip()
            if not p or p.startswith("#"):
                continue
            try:
                seen.add(int(p, 16))
            except ValueError:
                pass
    return seen


def write_file(path, addrs):
    os.makedirs(os.path.dirname(path), exist_ok=True) if os.path.dirname(path) else None
    with open(path, "w", newline="\n", encoding="utf-8") as f:
        f.write("# cdrtos_discovered.txt — trace-guided discovery seeds (in-ROM\n")
        f.write("# uncovered control-flow entries the static finder missed). One hex addr\n")
        f.write("# per line. Auto-unioned by tools/collect_seeds.py; re-seeded by\n")
        f.write("# CdiRecompBios. Regen + rebuild + re-run + collect until dry.\n")
        for a in sorted(addrs):
            f.write(f"{a:06X}\n")


def main():
    ap = argparse.ArgumentParser(description="union runtime uncovered entries into the seed file")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4380)
    ap.add_argument("--file", default=DEFAULT_FILE, help="seed file to union into")
    ap.add_argument("--dry-run", action="store_true", help="report new targets but don't write")
    args = ap.parse_args()

    try:
        r = query(args.host, args.port, {"cmd": "indirect_targets"})
    except (ConnectionRefusedError, OSError) as e:
        print(f"cannot reach debug server at {args.host}:{args.port} ({e})", file=sys.stderr)
        return 2
    if not r.get("ok"):
        print(json.dumps(r, indent=2)); return 1

    observed = [a for a in r.get("targets", [])]
    # Only even, in-ROM targets are seedable: an odd address is never a legal
    # 68000 instruction start (it would be an address error), and seeding one
    # corrupts the boundary split. The recompiler rejects odd too, but keep the
    # committed seed file clean.
    in_rom = sorted({a for a in observed if ROM_LO <= a < ROM_HI and a % 2 == 0})
    below = sorted({a for a in observed if a < ROM_LO})

    existing = read_existing(args.file)
    new = sorted(set(in_rom) - existing)

    print(f"observed uncovered entries: {len(observed)}")
    print(f"  in-ROM (seedable)       : {len(in_rom)}")
    print(f"  below-ROM (RAM-resident): {len(below)}  (not seeded; run interpreted)")
    print(f"already in {args.file}: {len(existing)}")
    print(f"NEW in-ROM targets      : {len(new)}")
    if new:
        preview = ", ".join(f"${a:06X}" for a in new[:16])
        print(f"  {preview}{' ...' if len(new) > 16 else ''}")

    if args.dry_run:
        print("(dry-run: not written)")
        return 0
    if new:
        write_file(args.file, existing | set(in_rom))
        print(f"wrote {len(existing | set(in_rom))} total seeds to {args.file}")
    else:
        print("no new targets — seed set is dry for this run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
