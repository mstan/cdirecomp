#!/usr/bin/env python3
"""Repeated cold boots through the real player shell, with no retries.

Every run uses normal pacing and its own battery/config/evidence directory.
The gate ends at the observed title card; campaign certification is separate.
"""
import argparse
import json
import re
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", type=Path)
    parser.add_argument("rom", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--port", type=int, default=4432)
    parser.add_argument("--count", type=int, default=30)
    parser.add_argument("--windowed", action="store_true")
    args = parser.parse_args()
    if args.count < 1:
        parser.error("count must be positive")
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = (args.generated_dir / "module_manifest.c").read_text()
    match = re.search(r"g_cdi_native_module_count\s*=\s*(\d+)", manifest)
    if not match or int(match[1]) < 1:
        parser.error("generated module manifest has no compiled identities")
    expected_modules = int(match[1])
    results = []
    tool = Path(__file__).with_name("hotelmario_scenario.py")
    for index in range(1, args.count + 1):
        directory = args.output / f"launch-{index:02d}"
        command = [sys.executable, str(tool), str(args.runtime), str(args.rom), str(args.disc),
                   "--output", str(directory), "--generated-dir", str(args.generated_dir),
                   "--port", str(args.port), "--speed", "1", "--seconds", "100",
                   "--require-build-provenance",
                   "--stop-video-hash", "1b902b2f985afb5f"]
        if args.windowed:
            command.append("--windowed")
        with (args.output / f"launch-{index:02d}.log").open("w") as log:
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        result = json.loads((directory / "result.json").read_text()) if (directory / "result.json").exists() else {}
        passed = completed.returncode == 0 and result.get("ok", False)
        evidence = json.loads((directory / "evidence.json").read_text()) if (directory / "evidence.json").exists() else {}
        native = evidence.get("native_state", {})
        passed = passed and native.get("compiled", 0) == expected_modules and native.get("active", 0) > 0
        results.append({"launch": index, "passed": passed, "result": result, "native": native})
        (args.output / "matrix.json").write_text(json.dumps({
            "passed": len(results) == args.count and all(r["passed"] for r in results), "requested": args.count,
            "expected_modules": expected_modules,
            "completed": len(results), "windowed": args.windowed,
            "gate": "normal-speed cold boot to pixel-exact title; no campaign claim", "runs": results}, indent=2))
        print(f"launch {index}/{args.count}: {'PASS' if passed else 'FAIL'}", flush=True)
        if not passed:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
