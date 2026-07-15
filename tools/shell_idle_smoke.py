#!/usr/bin/env python3
"""End-to-end smoke test for the live CD-RTOS shell idle boundary.

This is development instrumentation, not a player input frontend. It boots the
real recompiled BIOS, requires the OS-9 shell STOP with RULE 0a clean, verifies
that stopped device time is wall-clock paced, then publishes a button transition
and observes the real timed IKAT channel-A packet. The shell has not opened the
pointing-device driver yet, so it masks channel A. The test also requires that
the runtime respect that bit rather than forcing
an interrupt; other enabled IKAT channels are not part of this assertion.
"""
from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import time


SHELL_RESUME_PC = 0x40A3E6
INPUT_BTN1 = 1 << 4


def request(port: int, obj: dict, timeout: float = 5.0) -> dict:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    return json.loads(data.split(b"\n", 1)[0])


def wait_status(port: int, deadline: float) -> dict:
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            status = request(port, {"cmd": "status"}, timeout=1.0)
            if status.get("halted") == 1:
                return status
        except (OSError, ValueError) as exc:
            last_error = exc
        time.sleep(0.05)
    raise RuntimeError(f"runtime did not reach a queryable STOP ({last_error})")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)
    print(f"PASS  {message}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("runtime")
    ap.add_argument("rom")
    ap.add_argument("--port", type=int, default=4396)
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    args = ap.parse_args()

    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    proc = subprocess.Popen(
        [args.runtime, args.rom, "--port", str(args.port), "--headless"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        creationflags=creationflags,
    )
    try:
        first = wait_status(args.port, time.monotonic() + args.boot_timeout)
        check(first["pc"] == SHELL_RESUME_PC,
              f"shell halted with post-STOP PC ${SHELL_RESUME_PC:06X}")
        check(first["miss_count"] == 0, "RULE 0a dispatch-miss count is zero")
        check(first["irq_pending"] == 0, "shell begins with no pending IRQ")

        # Include the second TCP request in elapsed wall time: Python process
        # startup is outside this tool, and socket/query overhead is real time
        # during which the persistent runtime correctly continues ticking.
        start_frame = first["frame"]
        start = time.monotonic()
        time.sleep(1.0)
        paced = request(args.port, {"cmd": "status"})
        elapsed = time.monotonic() - start
        fps = (paced["frame"] - start_frame) / elapsed
        check(50.0 <= fps <= 70.0,
              f"STOP device clock is real-time paced ({fps:.2f} frames/s)")

        ikat_event_start = request(args.port, {"cmd": "ikat_events"})["total"]
        request(args.port, {"cmd": "set_input", "mask": INPUT_BTN1})
        deadline = time.monotonic() + 1.0
        ikat = {}
        while time.monotonic() < deadline:
            ikat = request(args.port, {"cmd": "emu_ikat_state"})
            regs = ikat.get("regs", [])
            remaining = ikat.get("out_remaining", [])
            if len(regs) == 15 and len(remaining) == 4 and remaining[0] == 4:
                break
            time.sleep(0.01)
        check(ikat["out_remaining"][0] == 4,
              "button transition produced a four-byte IKAT channel-A packet")
        check((ikat["regs"][12] & 0x02) != 0,
              "IKAT channel-A ISR bit is asserted")
        check((ikat["regs"][13] & 0x02) == 0,
              "no-media shell keeps unopened input channel A masked")

        new_ikat_events = [
            event for event in request(args.port, {"cmd": "ikat_events"})["events"]
            if event["seq"] >= ikat_event_start
        ]
        press_packets = [
            event for event in new_ikat_events
            if event["type"] == 2 and event["channel"] == 0
            and len(event["data"]) == 8
        ]
        check(any(int(event["data"][:2], 16) & 0x20
                  for event in press_packets),
              "left-button press is encoded in the IKAT packet")
        check(not any(event["type"] == 4 and event["channel"] == 0
                      for event in new_ikat_events),
              "masked channel-A event does not assert the IKAT level-2 line")

        release_event_start = request(args.port, {"cmd": "ikat_events"})["total"]
        request(args.port, {"cmd": "set_input", "mask": 0})
        release_packets = []
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            release_packets = [
                event for event in request(
                    args.port, {"cmd": "ikat_events"}
                )["events"]
                if event["seq"] >= release_event_start
                and event["type"] == 2 and event["channel"] == 0
                and len(event["data"]) == 8
            ]
            if release_packets:
                break
            time.sleep(0.01)
        check(any((int(event["data"][:2], 16) & 0x20) == 0
                  for event in release_packets),
              "left-button release is encoded in the next IKAT packet")
        print("shell-idle smoke: ALL PASS")
        return 0
    except Exception as exc:
        print(f"shell-idle smoke: FAIL: {exc}", file=sys.stderr)
        if proc.poll() is not None:
            out, err = proc.communicate()
            if out:
                print(out, file=sys.stderr)
            if err:
                print(err, file=sys.stderr)
        return 1
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
