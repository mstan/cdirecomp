#!/usr/bin/env python3
"""Export uncovered executed entries by immutable OS-9 image identity.

Only reads runtime diagnostics. Recompilation is offline; no guest code or
loader state is changed. Feed the result to CdiRecomp --module-seeds.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
from bios_options_smoke import request


def collect(port: int) -> dict:
    targets = []
    total = None
    while total is None or len(targets) < total:
        page = request(port, {"cmd": "module_targets", "from": len(targets), "count": 128})
        if not page.get("ok") or page.get("dropped", 0):
            raise RuntimeError("module entry ledger is unavailable or incomplete: "
                               f"ok={page.get('ok')}, total={page.get('total')}, "
                               f"dropped={page.get('dropped')}")
        total = page["total"]
        if len(targets) < total and not page["targets"]:
            raise RuntimeError("module entry ledger did not advance")
        targets.extend(page["targets"])
    return {"ok": True, "total": len(targets), "dropped": 0, "targets": targets}


def seed_text(evidence: dict) -> str:
    if not evidence.get("ok") or evidence.get("dropped", 0):
        raise ValueError("cannot export incomplete module entry evidence")
    entries = set()
    for target in evidence["targets"]:
        digest, offset = target["sha256"], target["offset"]
        if not re.fullmatch(r"[0-9a-fA-F]{64}", digest) or not isinstance(offset, int) or offset < 0 or offset > 0xFFFFFFFF or offset & 1:
            raise ValueError("invalid module identity or offset")
        entries.add((digest.lower(), offset))
    return "# Full OS-9 image SHA-256, executed entry offset (hex); no RAM addresses.\n" + "".join(
        f"{digest} 0x{offset:x}\n" for digest, offset in sorted(entries))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=4430)
    parser.add_argument("--evidence", type=Path, help="saved scenario evidence.json instead of a live runtime")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    evidence = json.loads(args.evidence.read_text())["module_targets"] if args.evidence else collect(args.port)
    with args.output.open("x", encoding="ascii") as stream:
        stream.write(seed_text(evidence))
    print(f"Exported {len(evidence['targets'])} entries to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
