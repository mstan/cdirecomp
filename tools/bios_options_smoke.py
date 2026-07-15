#!/usr/bin/env python3
"""Closeout smoke for battery persistence and BIOS Options/Storage navigation."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


SHELL_RESUME_PC = 0x40A3E6
RESET_FATAL_PC = 0x400636
INPUT_BTN1 = 1 << 4
CHA_IRQ_MASK = 0x02
# Side-effect-free framebuffer sampling.  Cover the complete visible surface so
# a menu action cannot escape the assertion merely because it redraws between
# a small set of hand-picked rows.
SAMPLE_ROWS = tuple(range(0, 240, 8))


def request(port: int, obj: dict, timeout: float = 3.0) -> dict:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall((json.dumps(obj) + "\n").encode())
        data = b""
        while b"\n" not in data:
            part = sock.recv(65536)
            if not part:
                break
            data += part
    return json.loads(data.split(b"\n", 1)[0])


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)
    print(f"PASS  {message}")


def require_live(proc: subprocess.Popen[str], state: dict) -> None:
    if proc.poll() is not None:
        raise RuntimeError(f"runtime exited ({proc.returncode})")
    if state.get("pc") == RESET_FATAL_PC:
        raise RuntimeError("BIOS entered the reset-vector fatal loop")
    if state.get("miss_count", 0) != 0:
        raise RuntimeError("BIOS options traversal produced a RULE 0a miss")


def wait_shell(proc: subprocess.Popen[str], port: int, deadline: float,
               minimum_frame: int = 0, minimum_insns: int = 0,
               require_driver: bool = True) -> dict:
    last: dict = {}
    while time.monotonic() < deadline:
        try:
            last = request(port, {"cmd": "status"}, timeout=1.0)
            require_live(proc, last)
            driver_ready = True
            if require_driver:
                ikat = request(port, {"cmd": "emu_ikat_state"}, timeout=1.0)
                driver_ready = (len(ikat.get("regs", [])) == 15 and
                                (ikat["regs"][13] & CHA_IRQ_MASK) != 0)
            if (last.get("halted") == 1 and
                    last.get("pc") == SHELL_RESUME_PC and
                    last.get("frame", 0) >= minimum_frame and
                    last.get("insns", 0) >= minimum_insns and driver_ready):
                return last
        except (OSError, ValueError, KeyError):
            if proc.poll() is not None:
                raise RuntimeError(f"runtime exited ({proc.returncode})")
        time.sleep(0.05)
    raise RuntimeError(f"BIOS did not return to shell STOP; last={last}")


def move_cursor(port: int, x: int, y: int) -> dict:
    for _ in range(20):
        video = request(port, {"cmd": "video_state"})
        dx = x - video["cursor_x"]
        dy = y - video["cursor_y"]
        if abs(dx) < 3 and abs(dy) < 3:
            return video
        request(port, {"cmd": "set_input", "mask": 0, "dx": dx, "dy": dy})
        time.sleep(0.15)
    raise RuntimeError(f"cursor did not reach ({x},{y}); last={video}")


def events_since(port: int, start: int) -> list[dict]:
    return [event for event in request(port, {"cmd": "ikat_events"})["events"]
            if event["seq"] >= start]


def click_with_frontend(port: int) -> tuple[dict, dict, list[dict]]:
    """Publish through the dev ABI until the physical frontend's next poll.

    The visible player intentionally owns the base input mask every 4 ms. The
    test repeats its dev state until one real 25-ms IKAT report samples it,
    then repeats release until the next report. This proves both edges without
    disabling or bypassing the player frontend.
    """
    start = request(port, {"cmd": "ikat_events"})["total"]
    deadline = time.monotonic() + 5.0
    press = None
    while time.monotonic() < deadline:
        request(port, {"cmd": "set_input", "mask": INPUT_BTN1})
        observed = events_since(port, start)
        press = next((event for event in observed
                      if event["type"] == 2 and event["channel"] == 0 and
                      (int(event["data"][:2], 16) & 0x20)), None)
        if press:
            break
        time.sleep(0.002)
    if not press:
        raise RuntimeError("left-button press did not reach IKAT")

    deadline = time.monotonic() + 5.0
    release = None
    while time.monotonic() < deadline:
        request(port, {"cmd": "set_input", "mask": 0})
        observed = events_since(port, start)
        release = next((event for event in observed
                        if event["type"] == 2 and event["channel"] == 0 and
                        event["seq"] > press["seq"] and
                        not (int(event["data"][:2], 16) & 0x20)), None)
        if release:
            return press, release, observed
        time.sleep(0.002)
    raise RuntimeError("left-button release did not reach IKAT")


def sampled_rows(port: int) -> tuple[bytes, ...]:
    rows = []
    for y in SAMPLE_ROWS:
        response = request(port, {"cmd": "video_scanline", "y": y})
        rows.append(bytes.fromhex(response["argb"]))
    return tuple(rows)


def changed_pixels(before: tuple[bytes, ...],
                   after: tuple[bytes, ...]) -> int:
    changed = 0
    for old, new in zip(before, after):
        changed += sum(old[index:index + 4] != new[index:index + 4]
                       for index in range(0, min(len(old), len(new)), 4))
    return changed


def select(proc: subprocess.Popen[str], port: int, target: tuple[int, int],
           before_state: dict, before_rows: tuple[bytes, ...],
           label: str, timeout: float) -> tuple[dict, tuple[bytes, ...]]:
    moved = move_cursor(port, *target)
    check(abs(moved["cursor_x"] - target[0]) < 3 and
          abs(moved["cursor_y"] - target[1]) < 3,
          f"{label} target was reached by relative IKAT motion")
    press, release, observed = click_with_frontend(port)
    check(press["seq"] < release["seq"],
          f"{label} emitted ordered button press/release reports")
    check(any(event["type"] == 5 and event["channel"] == 0
              for event in observed),
          f"the real pt1driv guest path drained the {label} click")
    state = wait_shell(
        proc, port, time.monotonic() + timeout,
        minimum_frame=before_state["frame"] + 1,
        minimum_insns=before_state["insns"] + 1000)
    time.sleep(1.0)
    rows = sampled_rows(port)
    changed = changed_pixels(before_rows, rows)
    check(changed >= 100,
          f"{label} changed the BIOS UI across sampled framebuffer rows")
    return state, rows


def player_environment(config_path: Path) -> dict[str, str]:
    environment = os.environ.copy()
    environment["CDI_PLAYER_CONFIG_PATH"] = str(config_path)
    environment["SDL_VIDEODRIVER"] = "dummy"
    environment["SDL_AUDIODRIVER"] = "dummy"
    return environment


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime")
    parser.add_argument("rom")
    parser.add_argument("disc", help="safe Mode-2 fixture; never launched")
    parser.add_argument("--port", type=int, default=4406)
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    try:
        with tempfile.TemporaryDirectory(prefix="cdirecomp-options-") as directory:
            root = Path(directory)
            config = root / "player.cfg"
            battery = root / "nvram.bin"
            config.write_text(
                "[input]\ncapture_mouse = false\n\n"
                "[rtc]\nsync_host_on_startup = false\n",
                encoding="utf-8",
            )
            environment = player_environment(config)

            initialized = subprocess.run(
                [args.runtime, args.rom, "--exit-on-stop",
                 "--port", str(args.port)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                timeout=args.timeout, env=environment,
                creationflags=creationflags,
            )
            init_output = initialized.stdout + initialized.stderr
            check(initialized.returncode == 0,
                  "first player boot exits cleanly at shell STOP")
            check("(new battery)" in init_output,
                  "first player boot creates a new battery image")
            check(battery.is_file() and battery.stat().st_size == 32768,
                  "battery image is exactly 32 KiB")
            battery_bytes = battery.read_bytes()
            check(any(value != 0xFF for value in battery_bytes),
                  "the real BIOS initialized persistent configuration bytes")

            proc = subprocess.Popen(
                [args.runtime, args.rom, "--disc", args.disc,
                 "--port", str(args.port)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                env=environment, creationflags=creationflags,
            )
            player_output = ""
            try:
                state = wait_shell(
                    proc, args.port, time.monotonic() + args.timeout,
                    minimum_frame=1000)
                rows = sampled_rows(args.port)
                check(state["miss_count"] == 0,
                      "initialized player shell is RULE 0a clean")

                state, options_rows = select(
                    proc, args.port, (140, 208), state, rows,
                    "Options", args.timeout)
                state, storage_rows = select(
                    proc, args.port, (384, 145), state, options_rows,
                    "empty Storage", args.timeout)
                state, options_rows = select(
                    proc, args.port, (140, 208), state, storage_rows,
                    "Options re-entry", args.timeout)
                state, exit_rows = select(
                    proc, args.port, (384, 210), state, options_rows,
                    "Options Exit", args.timeout)
                check(state["halted"] == 1 and state["pc"] == SHELL_RESUME_PC,
                      "Options/Storage/Exit traversal returned to shell STOP")
                check(state["miss_count"] == 0,
                      "complete Options traversal remains RULE 0a clean")
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(3)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                stdout, stderr = proc.communicate()
                player_output = stdout + stderr

            check("(loaded)" in player_output,
                  "second player boot loads the initialized battery image")

            before = battery.read_bytes()
            before_mtime = battery.stat().st_mtime_ns
            deterministic = subprocess.run(
                [args.runtime, args.rom, "--headless", "--exit-on-stop",
                 "--port", str(args.port)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                timeout=args.timeout, env=environment,
                creationflags=creationflags,
            )
            deterministic_output = deterministic.stdout + deterministic.stderr
            check(deterministic.returncode == 0,
                  "deterministic headless boot remains clean")
            check("[nvram]" not in deterministic_output and
                  battery.read_bytes() == before and
                  battery.stat().st_mtime_ns == before_mtime,
                  "deterministic profile neither loads nor rewrites player NVRAM")

        print("BIOS Options/NVRAM closeout smoke: ALL PASS")
        return 0
    except Exception as exc:
        print(f"BIOS Options/NVRAM closeout smoke: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
