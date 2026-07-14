#!/usr/bin/env python3
"""Exercise real CUE/BIN mount/eject through the shell's IKAT channel-D path."""
from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

SHELL_RESUME_PC = 0x40A3E6
RESET_FATAL_PC = 0x400636


def request(port: int, obj: dict, timeout: float = 5.0) -> dict:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall((json.dumps(obj) + "\n").encode())
        data = b""
        while b"\n" not in data:
            part = sock.recv(65536)
            if not part:
                break
            data += part
    return json.loads(data.split(b"\n", 1)[0])


def wait_shell(port: int, deadline: float, after_blocks: int = 0) -> dict:
    while time.monotonic() < deadline:
        try:
            state = request(port, {"cmd": "status"}, timeout=1.0)
            if (state.get("halted") == 1 and state.get("pc") == SHELL_RESUME_PC and
                    state.get("blocks", 0) > after_blocks):
                return state
        except (OSError, ValueError):
            pass
        time.sleep(0.05)
    raise RuntimeError("runtime did not return to the persistent shell STOP")


def check(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)
    print(f"PASS  {message}")


def observe_live(proc: subprocess.Popen[str], port: int, seconds: float) -> dict:
    """Keep observing after the first transient shell return.

    Media handling is asynchronous. A return to STOP only proves that one IRQ
    handler completed; later BIOS work can still leave the shell or fail. Keep
    the process alive long enough for that delayed work to mature, and reject
    the ROM's reset-vector fatal loop explicitly.
    """
    deadline = time.monotonic() + seconds
    last: dict = {}
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"runtime exited during media settling ({proc.returncode})")
        try:
            last = request(port, {"cmd": "status"}, timeout=1.0)
            if last.get("pc") == RESET_FATAL_PC:
                raise RuntimeError(f"BIOS entered reset-vector fatal loop ${RESET_FATAL_PC:06X}")
            if last.get("miss_count", 0) != 0:
                raise RuntimeError("media handling produced a RULE 0a dispatch miss")
        except (OSError, ValueError):
            if proc.poll() is not None:
                raise RuntimeError(f"runtime exited during media settling ({proc.returncode})")
        time.sleep(0.05)
    if not last:
        raise RuntimeError("runtime was not queryable during media settling")
    return last


def wait_guest_frames(proc: subprocess.Popen[str], port: int, start: int,
                      frames: int, deadline: float) -> dict:
    """Observe an equal amount of emulated time regardless of host load."""
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"runtime exited waiting for guest frames ({proc.returncode})")
        try:
            state = request(port, {"cmd": "status"}, timeout=1.0)
            if state.get("pc") == RESET_FATAL_PC:
                raise RuntimeError(f"BIOS entered reset-vector fatal loop ${RESET_FATAL_PC:06X}")
            if state.get("miss_count", 0) != 0:
                raise RuntimeError("media handling produced a RULE 0a dispatch miss")
            if state.get("frame", 0) >= start + frames:
                return state
        except (OSError, ValueError):
            pass
        time.sleep(0.05)
    raise RuntimeError(f"runtime did not advance {frames} guest frames")


def read_seed_file(path: Path) -> set[int]:
    seeds: set[int] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        value = line.strip()
        if value and not value.startswith("#"):
            seeds.add(int(value, 16))
    return seeds


def wait_media_events(proc: subprocess.Popen[str], port: int, start: int,
                      state: int, deadline: float) -> list[dict]:
    wanted = f"{state:02X}"
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"runtime exited waiting for media event ({proc.returncode})")
        try:
            events = [
                event for event in request(port, {"cmd": "ikat_events"})["events"]
                if event["seq"] >= start
            ]
            have_media = sum(event["type"] == 3 and event["channel"] == 3 and
                             event["data"] == wanted for event in events)
            have_irq = sum(event["type"] == 4 and event["channel"] == 3
                           for event in events)
            if have_media >= 1 and have_irq >= 1:
                return events
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for channel-D media state {wanted}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime")
    parser.add_argument("rom")
    parser.add_argument("disc")
    parser.add_argument("--port", type=int, default=4396)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--insert-frames", type=int, default=150,
                        help="emulated frames to observe after insertion")
    parser.add_argument("--eject-frames", type=int, default=75,
                        help="emulated frames to observe after ejection")
    parser.add_argument("--settle", type=float, default=0.0,
                        help="optional extra wall seconds to observe after insertion")
    parser.add_argument(
        "--seed-file", type=Path,
        default=Path(__file__).resolve().parents[1] / "bios" / "cdrtos_discovered.txt",
        help="trace-guided seed set that must cover every observed ROM entry",
    )
    args = parser.parse_args()

    proc = subprocess.Popen(
        [args.runtime, args.rom, "--port", str(args.port), "--headless"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    try:
        first = wait_shell(args.port, time.monotonic() + args.timeout)
        check(first["miss_count"] == 0, "initial shell is RULE 0a clean")

        insert_event_start = request(args.port, {"cmd": "ikat_events"})["total"]
        mounted = request(args.port, {"cmd": "mount_disc", "path": args.disc}, 15.0)
        check(mounted.get("present") == 1 and mounted.get("sectors", 0) > 16,
              "a real Mode-2 CUE/BIN image mounted")
        insert_events = wait_media_events(
            proc, args.port, insert_event_start, 1,
            time.monotonic() + args.timeout)
        inserted = wait_shell(args.port, time.monotonic() + args.timeout,
                              first["blocks"])
        check(sum(event["type"] == 3 and event["channel"] == 3 and
                  event["data"] == "01" for event in insert_events) == 1,
              "insertion published one IKAT channel-D media-change event")
        check(sum(event["type"] == 4 and event["channel"] == 3
                  for event in insert_events) == 1,
              "insertion asserted exactly one enabled IKAT channel-D IRQ")
        check(inserted["miss_count"] == 0,
              "insertion handler returned to shell with RULE 0a clean")
        settled = wait_guest_frames(
            proc, args.port, inserted["frame"], args.insert_frames,
            time.monotonic() + args.timeout)
        check(settled.get("pc") != RESET_FATAL_PC,
              f"delayed BIOS media work remains live for {args.insert_frames} guest frames")
        if args.settle > 0:
            observe_live(proc, args.port, args.settle)

        eject_event_start = request(args.port, {"cmd": "ikat_events"})["total"]
        ejected_reply = request(args.port, {"cmd": "eject_disc"})
        check(ejected_reply.get("present") == 0 and ejected_reply.get("sectors") == 0,
              "eject removed the mounted image")
        eject_events = wait_media_events(
            proc, args.port, eject_event_start, 0,
            time.monotonic() + args.timeout)
        eject_started = request(args.port, {"cmd": "status"})
        wait_guest_frames(
            proc, args.port, eject_started["frame"], args.eject_frames,
            time.monotonic() + args.timeout)
        ejected = wait_shell(args.port, time.monotonic() + args.timeout,
                             inserted["blocks"])
        check(sum(event["type"] == 3 and event["channel"] == 3 and
                  event["data"] == "00" for event in eject_events) == 1,
              "ejection published one IKAT channel-D media-change event")
        check(sum(event["type"] == 4 and event["channel"] == 3
                  for event in eject_events) == 1,
              "ejection asserted exactly one enabled IKAT channel-D IRQ")
        check(ejected["miss_count"] == 0, "ejection handler is RULE 0a clean")

        observed = request(args.port, {"cmd": "indirect_targets"})["targets"]
        existing = read_seed_file(args.seed_file)
        new_rom = sorted({addr for addr in observed
                          if 0x400000 <= addr < 0x500000 and addr % 2 == 0}
                         - existing)
        check(not new_rom,
              "real-media BIOS path is dry against the trace-guided ROM seed set"
              + ("; NEW=" + ",".join(f"${addr:06X}" for addr in new_rom[:16])
                 + ("..." if len(new_rom) > 16 else "") if new_rom else ""))
        print("disc insert/eject smoke: ALL PASS")
        return 0
    except Exception as exc:
        print(f"disc insert/eject smoke: FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
