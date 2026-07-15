#!/usr/bin/env python3
"""Prove four-way directional input navigates the real BIOS player shell.

The disc is only a valid media-state fixture: this test never presses a button
or launches its application. It waits for CD-RTOS to open pt1driv, publishes
directions through the timed IKAT path, and requires the guest driver to drain
every channel-A report and visibly update the MCD212 framebuffer. The packets
remain button-free and execution must return to the player-shell STOP. Passive
ready-media detection is allowed to issue IKAT drive reads.
"""
from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import time


SHELL_RESUME_PC = 0x40A3E6
RESET_FATAL_PC = 0x400636
INPUT_LEFT = 1 << 0
INPUT_UP = 1 << 1
INPUT_RIGHT = 1 << 2
INPUT_DOWN = 1 << 3
CHA_IRQ_MASK = 0x02
DIRECTIONS = (
    ("LEFT", INPUT_LEFT),
    ("DOWN", INPUT_DOWN),
    ("UP", INPUT_UP),
    ("RIGHT", INPUT_RIGHT),
)


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


def check(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)
    print(f"PASS  {message}")


def require_live(proc: subprocess.Popen[str], state: dict) -> None:
    if proc.poll() is not None:
        raise RuntimeError(f"runtime exited ({proc.returncode})")
    if state.get("pc") == RESET_FATAL_PC:
        raise RuntimeError(f"BIOS entered reset-vector fatal loop ${RESET_FATAL_PC:06X}")
    if state.get("miss_count", 0) != 0:
        raise RuntimeError("BIOS navigation produced a RULE 0a dispatch miss")


def wait_driver(proc: subprocess.Popen[str], port: int, deadline: float) -> tuple[dict, dict]:
    last_state: dict = {}
    last_ikat: dict = {}
    while time.monotonic() < deadline:
        try:
            last_state = request(port, {"cmd": "status"}, timeout=1.0)
            require_live(proc, last_state)
            last_ikat = request(port, {"cmd": "emu_ikat_state"}, timeout=1.0)
            if (last_state.get("halted") == 1 and
                    last_state.get("pc") == SHELL_RESUME_PC and
                    len(last_ikat.get("regs", [])) == 15 and
                    (last_ikat["regs"][13] & CHA_IRQ_MASK) != 0):
                return last_state, last_ikat
        except (OSError, ValueError, KeyError):
            if proc.poll() is not None:
                raise RuntimeError(f"runtime exited ({proc.returncode})")
        time.sleep(0.05)
    raise RuntimeError("CD-RTOS did not open pt1driv and return to the player-shell STOP")


def wait_cursor_ready(proc: subprocess.Popen[str], port: int,
                      deadline: float) -> dict:
    """Return a published frame plus the enabled MCD212 cursor position."""
    while time.monotonic() < deadline:
        state = request(port, {"cmd": "status"}, timeout=1.0)
        require_live(proc, state)
        video = request(port, {"cmd": "video_state"}, timeout=1.0)
        frame = request(port, {"cmd": "video_frame"}, timeout=1.0)
        if (video.get("cursor_enabled") == 1 and
                frame.get("generation", 0) > 0 and frame.get("argb_fnv1a")):
            return {**frame,
                    "cursor_x": video["cursor_x"],
                    "cursor_y": video["cursor_y"]}
        time.sleep(0.02)
    raise RuntimeError("player-shell MCD212 cursor did not become visible")


def moved_as_requested(name: str, before: dict, after: dict) -> bool:
    if name == "LEFT":
        return after["cursor_x"] < before["cursor_x"]
    if name == "RIGHT":
        return after["cursor_x"] > before["cursor_x"]
    if name == "UP":
        return after["cursor_y"] < before["cursor_y"]
    if name == "DOWN":
        return after["cursor_y"] > before["cursor_y"]
    raise ValueError(f"unknown direction {name}")


def packet_delta(event: dict) -> tuple[int, int] | None:
    """Decode one four-byte Class::Maneuvering report's signed X/Y delta."""
    if event["type"] != 2 or event["channel"] != 0 or len(event["data"]) < 8:
        return None
    packet = bytes.fromhex(event["data"][:8])
    x_raw = ((packet[0] & 0x03) << 6) | (packet[1] & 0x3F)
    y_raw = ((packet[0] & 0x0C) << 4) | (packet[2] & 0x3F)
    x = x_raw - 0x100 if x_raw & 0x80 else x_raw
    y = y_raw - 0x100 if y_raw & 0x80 else y_raw
    return x, y


def packet_moves_as_requested(name: str, event: dict) -> bool:
    delta = packet_delta(event)
    if delta is None:
        return False
    x, y = delta
    return ((name == "LEFT" and x < 0 and y == 0) or
            (name == "RIGHT" and x > 0 and y == 0) or
            (name == "UP" and y < 0 and x == 0) or
            (name == "DOWN" and y > 0 and x == 0))


def events_since(port: int, start: int) -> list[dict]:
    return [event for event in request(port, {"cmd": "ikat_events"})["events"]
            if event["seq"] >= start]


def exercise_direction(proc: subprocess.Popen[str], port: int, name: str,
                       mask: int, baseline: dict,
                       timeout: float,
                       relative: tuple[int, int] | None = None
                       ) -> tuple[dict, list[dict]]:
    event_start = request(port, {"cmd": "ikat_events"})["total"]
    observed_by_seq: dict[int, dict] = {}

    # Wall time is not device time: Debug builds can advance less than one
    # 25-ms IKAT cadence during a short host sleep, while optimized unpaced
    # builds can advance many. Query the always-on ring while input is held and
    # release after the first real report + enabled IRQ. The ordinary smoke
    # timeout remains the bounded failure guard.
    command = {"cmd": "set_input", "mask": mask}
    if relative is not None:
        command.update({"dx": relative[0], "dy": relative[1]})
    request(port, command)
    hold_deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < hold_deadline:
            for event in events_since(port, event_start):
                observed_by_seq[event["seq"]] = event
            held = observed_by_seq.values()
            if (any(packet_moves_as_requested(name, event)
                   for event in held) and
                    any(event["type"] == 4 and event["channel"] == 0
                        for event in held)):
                break
            time.sleep(0.001)
    finally:
        request(port, {"cmd": "set_input", "mask": 0})

    deadline = time.monotonic() + timeout
    saw_packet = False
    saw_irq = False
    saw_drain = False
    final_state: dict = {}
    observed: list[dict] = list(observed_by_seq.values())
    moved_cursor: dict | None = None
    rendered_cursor: dict | None = None
    move_generation = -1
    while time.monotonic() < deadline:
        final_state = request(port, {"cmd": "status"}, timeout=1.0)
        require_live(proc, final_state)
        for event in events_since(port, event_start):
            observed_by_seq[event["seq"]] = event
        observed = [observed_by_seq[seq] for seq in sorted(observed_by_seq)]
        saw_packet |= any(packet_moves_as_requested(name, event)
                          for event in observed)
        saw_irq |= any(event["type"] == 4 and event["channel"] == 0
                       for event in observed)
        live_ikat = request(port, {"cmd": "emu_ikat_state"})
        saw_drain |= saw_packet and live_ikat["out_remaining"][0] == 0
        video = request(port, {"cmd": "video_state"})
        frame = request(port, {"cmd": "video_frame"})
        cursor = {**frame,
                  "cursor_x": video["cursor_x"],
                  "cursor_y": video["cursor_y"]}
        if moved_cursor is None and moved_as_requested(name, baseline, cursor):
            moved_cursor = cursor
            move_generation = frame["generation"]
        if (moved_cursor is not None and
                frame["generation"] > move_generation and
                frame["argb_fnv1a"] != baseline["argb_fnv1a"]):
            rendered_cursor = cursor
        if (saw_packet and saw_irq and saw_drain and rendered_cursor and
                final_state.get("halted") == 1 and
                final_state.get("pc") == SHELL_RESUME_PC):
            break
        time.sleep(0.02)

    packets = [event for event in observed if packet_moves_as_requested(name, event)]
    check(saw_packet, f"{name} produced the requested timed IKAT channel-A report")
    check(saw_irq, f"{name} asserted the enabled IKAT level-2 line")
    check(saw_drain, f"the real pt1driv guest path drained {name}")
    check(all((int(event["data"][:2], 16) & 0x30) == 0
              for event in packets),
          f"{name} remained a button-free navigation packet")
    check(final_state.get("halted") == 1 and
          final_state.get("pc") == SHELL_RESUME_PC,
          f"{name} returned to the persistent player-shell STOP")
    check(moved_cursor is not None,
          f"{name} changed the MCD212 hardware-cursor coordinate")
    check(rendered_cursor is not None,
          f"{name} was published in the BIOS framebuffer")
    return rendered_cursor, observed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime")
    parser.add_argument("rom")
    parser.add_argument("disc", help="valid Mode-2 CUE/BIN fixture; never launched")
    parser.add_argument("--port", type=int, default=4396)
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()

    proc = subprocess.Popen(
        [args.runtime, args.rom, "--disc", args.disc,
         "--port", str(args.port), "--headless"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    try:
        state, ikat = wait_driver(proc, args.port,
                                  time.monotonic() + args.timeout)
        check(state["miss_count"] == 0, "ready-media player shell is RULE 0a clean")
        check((ikat["regs"][13] & CHA_IRQ_MASK) != 0,
              "CD-RTOS pt1driv enabled IKAT channel A")

        boot_events = request(args.port, {"cmd": "ikat_events"})["events"]
        check(any(event["type"] == 2 and event["channel"] == 3 and
                  event["data"] == "B0000215" for event in boot_events),
              "ready-media B0 response has the ROM-consumed byte layout")

        baseline = wait_cursor_ready(proc, args.port,
                                     time.monotonic() + args.timeout)
        navigation_events: list[dict] = []
        for name, mask in DIRECTIONS:
            baseline, observed = exercise_direction(
                proc, args.port, name, mask, baseline,
                args.timeout)
            navigation_events.extend(observed)

        baseline, mouse_events = exercise_direction(
            proc, args.port, "RIGHT", 0, baseline, args.timeout,
            relative=(24, 0))
        navigation_events.extend(mouse_events)
        check(any(packet_delta(event) == (24, 0)
                  for event in mouse_events),
              "relative mouse motion retained its exact IKAT delta")

        pointer_packets = [event for event in navigation_events
                           if event["type"] == 2 and event["channel"] == 0]
        check(pointer_packets and
              all((int(event["data"][:2], 16) & 0x30) == 0
                  for event in pointer_packets),
              "keyboard and mouse navigation never activated a player-shell button")
        final_state = request(args.port, {"cmd": "status"})
        check(final_state.get("miss_count") == 0,
              "keyboard and mouse navigation completed with RULE 0a clean")
        print("BIOS keyboard/mouse navigation smoke: ALL PASS")
        return 0
    except Exception as exc:
        print(f"BIOS navigation smoke: FAIL: {exc}", file=sys.stderr)
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
                proc.wait(3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
