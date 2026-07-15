#!/usr/bin/env python3
"""Exercise persistent one-shot host-local RTC synchronization at startup."""
from __future__ import annotations

import argparse
from datetime import datetime
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


RTC_LINE = re.compile(
    r"^\[rtc\] startup seed "
    r"(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{2}) "
    r"\(host local; one shot\)$",
    re.MULTILINE,
)


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)
    print(f"PASS  {message}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime")
    parser.add_argument("rom")
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="cdirecomp-rtc-") as directory:
        config_path = Path(directory) / "player.cfg"
        config_text = (
            "[input]\n"
            "capture_mouse = false\n\n"
            "[rtc]\n"
            "sync_host_on_startup = true\n"
        )
        config_path.write_text(config_text, encoding="utf-8")
        environment = os.environ.copy()
        environment["CDI_PLAYER_CONFIG_PATH"] = str(config_path)
        environment["SDL_VIDEODRIVER"] = "dummy"
        environment["SDL_AUDIODRIVER"] = "dummy"
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            completed = subprocess.run(
                [args.runtime, args.rom, "--exit-on-stop"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=args.timeout,
                env=environment,
                creationflags=creationflags,
            )
            output = completed.stdout + completed.stderr
            check(completed.returncode == 0, "player-profile boot exits cleanly")
            check("capture_mouse=off, rtc_sync=on" in output,
                  "persistent config enables RTC without enabling capture")
            matches = list(RTC_LINE.finditer(output))
            check(len(matches) == 1, "host-local RTC is seeded exactly once")
            fields = [int(value) for value in matches[0].groups()]
            seeded = datetime(*fields[:6], microsecond=fields[6] * 10_000)
            skew = abs((datetime.now() - seeded).total_seconds())
            check(skew <= 120.0,
                  f"seed matches current host-local time ({skew:.2f}s skew)")
            check("CPU halted (STOP) after " in output and
                  "PC=$0040A3E6" in output and
                  "dispatch miss(es)" not in output,
                  "RTC-enabled boot reaches the canonical shell STOP cleanly")
            check(config_path.read_text(encoding="utf-8") == config_text,
                  "runtime preserves the durable preference file")
            print("RTC-startup smoke: ALL PASS")
            return 0
        except Exception as exc:
            print(f"RTC-startup smoke: FAIL: {exc}", file=sys.stderr)
            if "output" in locals() and output:
                print(output, file=sys.stderr)
            return 1


if __name__ == "__main__":
    raise SystemExit(main())
