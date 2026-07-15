#!/usr/bin/env python3
"""Drive Hotel Mario from the real player shell into its attract sequence.

The optional acceptance mode gates the clean title-card frame, bumper audio,
disc progress, and native-dispatch health without bundling either user asset.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from bios_options_smoke import (
    check,
    click_with_frontend,
    move_cursor,
    player_environment,
    request,
)


PLAY_TARGET = (630, 165)
SHELL_RESUME_PC = 0x40A3E6
ATTRACT_TITLE_HASH = "1b902b2f985afb5f"
BUMPER_AUDIO_HASH = "6a1da0355395863e"


def wait_shell_traced(proc: subprocess.Popen[str], port: int,
                      deadline: float, minimum_frame: int) -> dict:
    """Wait for STOP while retaining the last live hardware rings on failure."""
    last: dict = {}
    last_ikat: dict = {}
    last_ciap: dict = {}
    while time.monotonic() < deadline:
        try:
            last = request(port, {"cmd": "status"}, timeout=1.0)
            last_ikat = request(port, {"cmd": "ikat_events"}, timeout=1.0)
            last_ciap = request(
                port, {"cmd": "ciap_events", "count": 40}, timeout=1.0)
            driver = request(port, {"cmd": "emu_ikat_state"}, timeout=1.0)
            if (last.get("halted") == 1 and
                    last.get("pc") == SHELL_RESUME_PC and
                    last.get("frame", 0) >= minimum_frame and
                    len(driver.get("regs", [])) == 15 and
                    (driver["regs"][13] & 0x02)):
                return last
        except (OSError, ValueError, KeyError):
            if proc.poll() is not None:
                break
        time.sleep(0.05)
    raise RuntimeError(
        "BIOS did not reach shell STOP; "
        f"last={last} ikat={last_ikat} ciap={last_ciap}")


def save_ppm(port: int, path: Path) -> None:
    state = request(port, {"cmd": "video_state"})
    first = request(port, {"cmd": "video_scanline", "y": 0})
    width = first["width"]
    height = first["height"]
    rgb = bytearray()
    for y in range(height):
        row = first if y == 0 else request(
            port, {"cmd": "video_scanline", "y": y})
        argb = bytes.fromhex(row["argb"])
        for index in range(0, len(argb), 4):
            rgb.extend(argb[index + 1:index + 4])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + rgb)
    print(f"WROTE screenshot {path} cursor=({state['cursor_x']},{state['cursor_y']})")


def ciap_register_events(port: int) -> dict:
    tail = request(port, {"cmd": "ciap_events", "count": 1})
    total = tail["total"]
    events = []
    start = tail["oldest"]
    while start < total:
        page = request(port, {"cmd": "ciap_events", "from": start,
                              "count": 256})
        if not page["events"]:
            break
        events.extend(event for event in page["events"]
                      if (0x2580 <= event["offset"] < 0x2600 or
                          event["offset"] == 0x3FFE))
        start = page["events"][-1]["seq"] + 1
    return {"ok": True, "total": total, "register_events": events}


def disc_bin_path(disc: Path) -> Path:
    if disc.suffix.lower() != ".cue":
        return disc
    for line in disc.read_text(encoding="utf-8-sig").splitlines():
        stripped = line.strip()
        if stripped.upper().startswith("FILE "):
            name = stripped[5:].rsplit(" ", 1)[0].strip().strip('"')
            return disc.parent / name
    raise RuntimeError(f"no FILE entry in {disc}")


def report_dma_payloads(port: int, disc: Path) -> None:
    stores = request(port, {"cmd": "stores", "addr": 0x8000400C,
                            "count": 64})["records"]
    destinations = sorted({record["val"] for record in stores})
    expected: dict[str, int] = {}
    candidate_payloads: dict[int, bytes] = {}
    with disc_bin_path(disc).open("rb") as stream:
        for lba in [*range(0, 40), *range(2250, 2300),
                    *range(3180, 3260)]:
            stream.seek(lba * 2352 + 24)
            payload = stream.read(2048)
            expected[hashlib.sha1(payload).hexdigest()] = lba
            candidate_payloads[lba] = payload
    reports = []
    for address in destinations:
        memory = request(port, {"cmd": "read_mem", "addr": address,
                                "len": 2048})["bytes"]
        if "-" in memory:
            reports.append({"address": address, "readable": False})
            continue
        digest = hashlib.sha1(bytes.fromhex(memory)).hexdigest()
        actual = bytes.fromhex(memory)
        closest_lba, matching_bytes = max(
            ((lba, sum(a == b for a, b in zip(actual, payload)))
             for lba, payload in candidate_payloads.items()),
            key=lambda item: item[1],
        )
        reports.append({"address": address, "sha1": digest,
                        "matching_lba": expected.get(digest),
                        "closest_lba": closest_lba,
                        "matching_bytes": matching_bytes,
                        "mismatches": [
                            {"offset": index, "actual": actual[index],
                             "expected": candidate_payloads[closest_lba][index]}
                            for index in range(len(actual))
                            if actual[index] !=
                            candidate_payloads[closest_lba][index]
                        ][:20]})
    print("DMA_PAYLOADS " + json.dumps(reports, sort_keys=True))


def report_ram_modules(port: int) -> None:
    modules = []
    needles = (b"cdi_hotel", b"cdi_hotel_data", b"cdi_hotel.stb",
               b"intro_sub.o", b"do_intro", b"cdi_intro.map")
    strings = {needle.decode("ascii"): [] for needle in needles}
    segments = []
    for base in (0, 0x200000):
        ram = bytearray()
        for address in range(base, base + 0x80000, 4096):
            response = request(port, {"cmd": "read_mem", "addr": address,
                                      "len": 4096})["bytes"]
            if "-" in response:
                raise RuntimeError(f"unreadable RAM page at {address:#x}")
            ram.extend(bytes.fromhex(response))
        segments.append((base, ram))
    for base, ram in segments:
        cursor = 0
        while True:
            cursor = ram.find(b"\x4a\xfc", cursor)
            if cursor < 0:
                break
            size = int.from_bytes(ram[cursor + 4:cursor + 8], "big")
            name_offset = int.from_bytes(ram[cursor + 12:cursor + 16], "big")
            name = bytearray()
            if (32 <= size <= len(ram) - cursor and
                    0 < name_offset < min(size, 0x10000)):
                position = cursor + name_offset
                while position < cursor + size and len(name) < 128:
                    value = ram[position]
                    name.append(value & 0x7f)
                    position += 1
                    if value & 0x80:
                        break
                if name and name[-1] != 0:
                    modules.append({"address": base + cursor, "size": size,
                                    "name": name.decode("latin-1")})
            cursor += 2
    for needle in needles:
        for base, ram in segments:
            cursor = 0
            while len(strings[needle.decode("ascii")]) < 16:
                cursor = ram.find(needle, cursor)
                if cursor < 0:
                    break
                strings[needle.decode("ascii")].append(base + cursor)
                cursor += 1
    print("RAM_MODULES " + json.dumps(
        {"modules": modules, "hotel_strings": strings}, sort_keys=True))


def report_initial_video_control(port: int) -> None:
    pages: dict[int, bytes] = {}

    def word(address: int) -> int:
        page_address = address & ~0xFFF
        if page_address not in pages:
            response = request(port, {"cmd": "read_mem",
                                      "addr": page_address, "len": 4096})
            pages[page_address] = bytes.fromhex(response["bytes"])
        offset = address - page_address
        return int.from_bytes(pages[page_address][offset:offset + 4], "big")

    for plane, start in (("a", 0x400), ("b", 0x200400)):
        cursor = start
        bank = 0
        events = []
        colors = Counter()
        color_samples: dict[int, list[int]] = {}
        for _ in range(8192):
            instruction = word(cursor)
            address = cursor
            cursor += 4
            opcode = instruction >> 28
            command = instruction >> 24
            if command == 0xC3:
                bank = instruction & 3
                events.append({"address": address, "word": instruction,
                               "bank": bank})
            elif 0x80 <= command <= 0xBF:
                colors[bank] += 1
                color_samples.setdefault(bank, []).append(
                    instruction & 0x00FFFFFF)
            if opcode == 4:
                cursor = instruction & 0x003FFFFF
                events.append({"address": address, "word": instruction,
                               "jump": cursor})
            elif opcode in (0, 3, 5):
                events.append({"address": address, "word": instruction,
                               "stop": opcode})
                break
        print("VIDEO_CONTROL " + json.dumps({
            "plane": plane, "colors_by_bank": dict(colors),
            "color_samples": color_samples,
            "events": events,
        }, sort_keys=True))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime")
    parser.add_argument("rom")
    parser.add_argument("disc")
    parser.add_argument("--port", type=int, default=4410)
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--timeout", type=float, default=75.0)
    parser.add_argument("--screenshot", type=Path)
    parser.add_argument("--compact", action="store_true")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--calls-only", action="store_true")
    parser.add_argument("--stop-on-final-c4", action="store_true")
    parser.add_argument("--break-frame", type=int)
    parser.add_argument("--unpaced", action="store_true")
    parser.add_argument("--no-click", action="store_true")
    parser.add_argument("--accept-attract", action="store_true")
    parser.add_argument("--video-control", action="store_true")
    args = parser.parse_args()
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    with tempfile.TemporaryDirectory(prefix="cdirecomp-hotel-") as directory:
        root = Path(directory)
        config = root / "player.cfg"
        config.write_text(
            "[input]\ncapture_mouse = false\n\n"
            "[rtc]\nsync_host_on_startup = false\n",
            encoding="utf-8",
        )
        environment = player_environment(config)
        if args.unpaced:
            environment["CDI_PACE_SPEED"] = "8"

        initialized = subprocess.run(
            [args.runtime, args.rom, "--disc", args.disc,
             "--exit-on-stop", "--port", str(args.port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            timeout=args.timeout, env=environment, creationflags=creationflags,
        )
        check(initialized.returncode == 0, "fresh player battery initialized")

        runtime_args = [args.runtime, args.rom, "--disc", args.disc,
                        "--port", str(args.port)]
        if args.break_frame is not None:
            runtime_args.extend(("--stop-frame", str(args.break_frame)))
        proc = subprocess.Popen(
            runtime_args,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env=environment, creationflags=creationflags,
        )
        try:
            try:
                shell = wait_shell_traced(
                    proc, args.port, time.monotonic() + args.timeout,
                    minimum_frame=1000)
            except RuntimeError:
                if proc.poll() is None:
                    for command in (
                        {"cmd": "status"},
                        {"cmd": "get_registers"},
                        {"cmd": "indirect_targets"},
                        {"cmd": "dispatch_miss_info"},
                        {"cmd": "video_state"},
                        {"cmd": "stores", "addr": 0x80004000, "count": 16},
                        {"cmd": "stores", "addr": 0x80004005, "count": 16},
                        {"cmd": "stores", "addr": 0x80004007, "count": 16},
                        {"cmd": "stores", "addr": 0x8000400A, "count": 16},
                        {"cmd": "stores", "addr": 0x8000400C, "count": 16},
                        {"cmd": "stores", "addr": 0x80004014, "count": 16},
                    ):
                        print("PREOPEN_" + command["cmd"].upper() + " " +
                              json.dumps(request(args.port, command),
                                         sort_keys=True))
                    if args.screenshot:
                        save_ppm(args.port, args.screenshot)
                raise
            print("SHELL " + json.dumps(shell, sort_keys=True))
            if not args.no_click:
                moved = move_cursor(args.port, *PLAY_TARGET)
                check(abs(moved["cursor_x"] - PLAY_TARGET[0]) < 3 and
                      abs(moved["cursor_y"] - PLAY_TARGET[1]) < 3,
                      "Play CD-i target reached through relative IKAT movement")
                press, release, observed = click_with_frontend(args.port)
                check(press["seq"] < release["seq"],
                      "Play CD-i emitted ordered button edges")
                check(any(event["type"] == 5 and event["channel"] == 0
                          for event in observed),
                      "pt1driv drained the Play CD-i click")

            deadline = time.monotonic() + args.seconds
            play_started = time.monotonic()
            play_frame = shell["frame"]
            previous = None
            observed_reset = None
            video_transitions = []
            previous_video_hash = None
            title_observed = None
            bumper_audio_observed = None
            late_background_observed = None
            latest_audio = None
            latest_ciap = None
            while time.monotonic() < deadline and proc.poll() is None:
                try:
                    state = request(args.port, {"cmd": "status"}, timeout=1.0)
                    summary = (state.get("pc"), state.get("frame"),
                               state.get("insns"), state.get("halted"),
                               state.get("miss_count"))
                    if not args.quick and summary != previous:
                        print("STATE " + json.dumps(state, sort_keys=True))
                        previous = summary
                    if args.quick:
                        if (args.break_frame is not None and
                                state.get("frame", 0) >= args.break_frame):
                            break
                        if state.get("frame", 0) >= play_frame + 500:
                            video = request(
                                args.port, {"cmd": "video_frame"}, timeout=1.0)
                            current_hash = video.get("argb_fnv1a")
                            if current_hash != previous_video_hash:
                                video_transitions.append({
                                    "frame": state.get("frame"),
                                    "hash": current_hash,
                                })
                                previous_video_hash = current_hash
                            if current_hash == ATTRACT_TITLE_HASH:
                                title_observed = {
                                    "frame": state.get("frame"),
                                    "hash": current_hash,
                                }
                            latest_audio = request(
                                args.port, {"cmd": "audio_state"}, timeout=1.0)
                            if (latest_audio.get("pcm_fnv1a") ==
                                    BUMPER_AUDIO_HASH and
                                    latest_audio.get("sectors") == 149 and
                                    latest_audio.get("dropped_frames") == 0):
                                bumper_audio_observed = {
                                    "frame": state.get("frame"),
                                    "hash": latest_audio["pcm_fnv1a"],
                                    "sectors": latest_audio["sectors"],
                                }
                        if state.get("frame", 0) >= play_frame + 1700:
                            latest_ciap = request(
                                args.port, {"cmd": "ciap_state"}, timeout=1.0)
                            video_state = request(
                                args.port, {"cmd": "video_state"}, timeout=1.0)
                            clut = request(
                                args.port, {"cmd": "video_clut"}, timeout=1.0)
                            background_colors = sum(
                                (color & 0x00FFFFFF) != 0
                                for color in clut.get("colors", [])[128:])
                            if (latest_ciap.get("drive_lba", 0) >= 4650 and
                                    video_state.get("plane_line_nonblack_b", 0)
                                    >= 700 and background_colors >= 64):
                                late_background_observed = {
                                    "frame": state.get("frame"),
                                    "line_nonblack": video_state[
                                        "plane_line_nonblack_b"],
                                    "clut_nonblack": background_colors,
                                    "line_hash": video_state[
                                        "plane_line_fnv1a_b"],
                                }
                        ikat = request(
                            args.port, {"cmd": "ikat_events"}, timeout=1.0)
                        commands = [event for event in ikat["events"]
                                    if event["type"] == 1]
                        observed_reset = next((
                            event for event in reversed(commands)
                            if event["channel"] == 2 and
                            event["data"] == "88"), None)
                        last_a1 = next((
                            event for event in reversed(commands)
                            if event["channel"] == 3 and
                            event["data"].startswith("A1") and
                            event["frame"] >= 1600), None)
                        final_c4 = next((
                            event for event in reversed(commands)
                            if event["channel"] == 3 and
                            event["data"].startswith("C4") and
                            last_a1 and event["seq"] > last_a1["seq"]), None)
                        if args.stop_on_final_c4 and final_c4:
                            break
                        if (args.accept_attract and title_observed and
                                bumper_audio_observed and
                                late_background_observed and latest_audio and
                                latest_audio.get("sectors", 0) > 149 and
                                latest_audio.get("dropped_frames") == 0):
                            break
                        if observed_reset:
                            break
                except (OSError, ValueError):
                    if proc.poll() is not None:
                        break
                time.sleep(1.0)

            if proc.poll() is None:
                request(args.port, {"cmd": "pause"})
                time.sleep(0.1)
                if args.quick:
                    print("VIDEO_TRANSITIONS " + json.dumps(
                        video_transitions, sort_keys=True))
                    quick_state = {}
                    for command in ({"cmd": "status"},
                                    {"cmd": "get_registers"},
                                    {"cmd": "disc_state"},
                                    {"cmd": "ciap_state"},
                                    {"cmd": "dispatch_miss_info"},
                                    {"cmd": "video_frame"},
                                    {"cmd": "video_state"},
                                    {"cmd": "video_clut"},
                                    {"cmd": "audio_state"}):
                        response = request(args.port, command)
                        quick_state[command["cmd"]] = response
                        print(command["cmd"].upper() + " " + json.dumps(
                            response, sort_keys=True))
                    if args.video_control:
                        report_initial_video_control(args.port)
                        for address, name in ((0x8000400A, "mtc"),
                                              (0x8000400C, "mar"),
                                              (0x00076A94, "clut_a0"),
                                              (0x0027B814, "clut_b128"),
                                              (0x0027B918, "clut_b192"),
                                              (0x0027BBE4, "clut_source_b"),
                                              (0x0000703C, "clut_stage_b")):
                            state = request(args.port, {
                                "cmd": "stores", "addr": address,
                                "count": 64,
                            })
                            print("VIDEO_DMA " + json.dumps({
                                "register": name,
                                "state": state,
                            }, sort_keys=True))
                            if name == "clut_stage_b" and state["records"]:
                                record = state["records"][-1]
                                before = 48 if name == "clut_stage_b" else 4
                                print("CLUT_WRITE_TRACE " + json.dumps(
                                    request(args.port, {
                                        "cmd": "trace",
                                        "from": max(0, record["seq"] - before),
                                        "count": before + 10,
                                    }), sort_keys=True))
                        print("VIDEO_DMA " + json.dumps({
                            "register": "dmactl",
                            "state": request(args.port, {
                                "cmd": "ciap_events", "offset": 0x25C2,
                                "count": 256,
                            }),
                        }, sort_keys=True))
                        for offset, name in ((0x2588, "tacs"),
                                             (0x258A, "aacs"),
                                             (0x258C, "tcm1"),
                                             (0x258E, "acm1"),
                                             (0x2590, "acm2"),
                                             (0x2592, "file"),
                                             (0x2596, "ccr")):
                            state = request(args.port, {
                                "cmd": "ciap_events", "offset": offset,
                                "count": 64,
                            })
                            print("VIDEO_SELECT " + json.dumps({
                                "register": name,
                                "events": [event for event in state["events"]
                                           if event["write"]][-16:],
                            }, sort_keys=True))
                    if args.accept_attract:
                        status = quick_state["status"]
                        ciap = quick_state["ciap_state"]
                        dispatch = quick_state["dispatch_miss_info"]
                        video = quick_state["video_frame"]
                        audio = quick_state["audio_state"]
                        elapsed = max(0.001, time.monotonic() - play_started)
                        fields_per_second = ((status["frame"] - play_frame) /
                                             elapsed)
                        print("ATTRACT_OBSERVATIONS " + json.dumps({
                            "title": title_observed,
                            "bumper_audio": bumper_audio_observed,
                            "late_background": late_background_observed,
                            "fields_per_second": fields_per_second,
                        }, sort_keys=True))
                        check(title_observed is not None,
                              "attract title framebuffer was pixel-exact")
                        check(late_background_observed is not None,
                              "later attract scene has a populated background plane")
                        check(ciap["last_lba"] >= 4650,
                              "attract stream advanced into the animated intro")
                        check(dispatch["count"] == 0,
                              "attract path has zero native dispatch misses")
                        check(bumper_audio_observed is not None and
                              audio["sectors"] > 149 and
                              audio["dropped_frames"] == 0,
                              "bumper and intro XA audio are clean with zero drops")
                        check(fields_per_second >= 55.0,
                              "attract playback sustains real-time field pacing")
                    if args.break_frame is None:
                        trace = request(
                            args.port, {"cmd": "trace", "count": 96})
                        print("TRACE_TAIL " + json.dumps(trace,
                                                           sort_keys=True))
                    ikat = request(args.port, {"cmd": "ikat_events"})
                    commands = [event for event in ikat["events"]
                                if event["type"] == 1]
                    print("IKAT_COMMANDS " + json.dumps(
                        {"total": ikat["total"], "commands": commands},
                        sort_keys=True))
                    if args.break_frame is not None:
                        print("IKAT_EVENTS " + json.dumps(ikat,
                                                           sort_keys=True))
                        final_c4 = next((
                            event for event in reversed(commands)
                            if event["channel"] == 3 and
                            event["data"].startswith("C4")), None)
                        if final_c4 and not args.unpaced:
                            for event in ikat["events"]:
                                if event["seq"] > final_c4["seq"]:
                                    print("IKAT_RESPONSE_TRACE " +
                                          json.dumps(request(args.port, {
                                              "cmd": "trace",
                                              "from": max(
                                                  0,
                                                  event["trace_seq"] - 12),
                                              "count": 96,
                                          }), sort_keys=True))
                    drive_controls = [event for event in commands
                                      if event["channel"] == 3 and
                                      event["data"][:2] in ("C3", "C4", "C5")]
                    command_calls = []
                    for event in drive_controls[-6:]:
                        call = request(
                            args.port,
                            {"cmd": "trace", "from": event["trace_seq"] - 10,
                             "count": 16})
                        command_calls.append({
                            "command": event["data"],
                            "frame": event["frame"],
                            "records": [
                                {key: record[key] for key in
                                 ("seq", "pc", "op", "d0", "d1", "a0",
                                  "a1", "a2", "a7", "a7top")}
                                for record in call["records"]
                            ],
                        })
                    if args.break_frame is None:
                        print("DRIVE_CONTROL_CALLS " + json.dumps(
                            command_calls, sort_keys=True))
                    if args.calls_only:
                        for offset in (0x2596, 0x25A6, 0x25AA):
                            ciap_tail = request(args.port, {
                                "cmd": "ciap_events", "count": 256,
                                "offset": offset, "frame_min": 1550,
                            })
                            print(f"CIAP_{offset:04X} " + json.dumps(
                                ciap_tail, sort_keys=True))
                            if offset == 0x25A6 and not args.unpaced:
                                for event in ciap_tail["events"][-12:]:
                                    print("APCR_TRACE " + json.dumps(request(
                                        args.port, {
                                            "cmd": "trace",
                                            "from": max(0,
                                                        event["trace_seq"] - 4),
                                            "count": 12,
                                        }), sort_keys=True))
                        for address in (0x6300, 0xB800, 0x275200, 0x275B80,
                                        0x2760C0, 0x276280, 0x2765C0,
                                        0x278F80, 0x27E000, 0x7C2C0,
                                        0x220480):
                            print("RAM_WINDOW " + json.dumps(request(
                                args.port,
                                {"cmd": "read_mem", "addr": address,
                                 "len": 256}), sort_keys=True))
                        print("INTRO_MODULE " + json.dumps(request(
                            args.port,
                            {"cmd": "read_mem", "addr": 0x21FC80,
                             "len": 0x80}), sort_keys=True))
                        for address, length in ((0x4000, 0x180),
                                                (0x5180, 0x60),
                                                (0x61E0, 0x40),
                                                (0x27E870, 0x40),
                                                (0x27E900, 0x30)):
                            print("ASYNC_STATE " + json.dumps(request(
                                args.port,
                                {"cmd": "read_mem", "addr": address,
                                 "len": length}), sort_keys=True))
                        for address in (0x402A, 0x4124, 0x4128, 0x412C,
                                        0x4130, 0x4134, 0x4135,
                                        0x4B52, 0x51AE, 0x61E8,
                                        0x6204, 0x27E878, 0x27E90A, 0x27E90C,
                                        0x27E90D, 0x27E90F, 0x27E910):
                            stores = request(
                                args.port,
                                {"cmd": "stores", "addr": address,
                                 "count": 64})
                            print("ASYNC_STORES " + json.dumps(
                                stores, sort_keys=True))
                            if address == 0x27E878:
                                for record in stores["records"][-4:]:
                                    print("EVENT_SIGNAL_TRACE " + json.dumps(
                                        request(args.port, {
                                            "cmd": "trace",
                                            "from": max(0, record["seq"] - 16),
                                            "count": 96,
                                        }), sort_keys=True))
                        if args.screenshot:
                            save_ppm(args.port, args.screenshot)
                        return 0
                    reset = observed_reset or next((
                        event for event in reversed(commands)
                        if event["channel"] == 2 and
                        event["data"] == "88"), None)
                    if reset:
                        trace = request(
                            args.port,
                            {"cmd": "trace",
                             "from": max(0, reset["trace_seq"] - 32),
                             "count": 48},
                        )
                        print("RESET_TRACE " + json.dumps([
                            {key: record[key] for key in
                             ("seq", "pc", "op", "sr", "d0", "d1",
                              "d2", "d3", "a0", "a1", "a2", "a7",
                              "a7top", "frame")}
                            for record in trace["records"]
                        ], sort_keys=True))
                        ram_tail = []
                        scan = max(0, reset["trace_seq"] - 70000)
                        while scan < reset["trace_seq"]:
                            page = request(
                                args.port,
                                {"cmd": "trace", "from": scan,
                                 "count": min(256,
                                              reset["trace_seq"] - scan)},
                            )
                            records = page["records"]
                            ram_tail.extend(
                                record for record in records
                                if 0x10000 <= record["pc"] < 0x400000)
                            ram_tail = ram_tail[-24:]
                            if not records:
                                break
                            scan = records[-1]["seq"] + 1
                        print("LAST_RAM_TRACE " + json.dumps([
                            {key: record[key] for key in
                             ("seq", "pc", "op", "sr", "d0", "d1",
                              "d2", "d3", "a0", "a1", "a2", "a7",
                              "a7top", "frame")}
                            for record in ram_tail
                        ], sort_keys=True))
                        if ram_tail:
                            last_ram = ram_tail[-1]
                            transition = request(
                                args.port,
                                {"cmd": "trace",
                                 "from": max(0, last_ram["seq"] - 8),
                                 "count": 64},
                            )
                            print("RAM_EXIT_TRACE " + json.dumps([
                                {key: record[key] for key in
                                 ("seq", "pc", "op", "sr", "d0", "d1",
                                  "d2", "d3", "a0", "a1", "a2", "a7",
                                  "a7top", "frame")}
                                for record in transition["records"]
                            ], sort_keys=True))
                            code_addr = max(0x10000, last_ram["pc"] - 96)
                            print("RAM_EXIT_CODE " + json.dumps(request(
                                args.port,
                                {"cmd": "read_mem", "addr": code_addr,
                                 "len": 192}), sort_keys=True))
                    ciap = ciap_register_events(args.port)
                    print("CIAP_SELECTION " + json.dumps([
                        {key: event[key] for key in
                         ("seq", "frame", "pc", "offset", "value", "write")}
                        for event in ciap["register_events"]
                        if event["write"] and event["offset"] in
                        (0x2588, 0x258A, 0x258C, 0x2590, 0x2592)
                    ], sort_keys=True))
                    if args.screenshot:
                        save_ppm(args.port, args.screenshot)
                    return 0
                for command in ({"cmd": "disc_state"},
                                {"cmd": "dispatch_miss_info"},
                                {"cmd": "video_frame"},
                                {"cmd": "audio_state"},
                                {"cmd": "video_state"},
                                {"cmd": "read_mem", "addr": 0,
                                 "len": 16},
                                {"cmd": "stores", "addr": 0x8000400A,
                                 "count": 64},
                                {"cmd": "stores", "addr": 0x8000400C,
                                 "count": 64},
                                {"cmd": "read_mem", "addr": 517696,
                                 "len": 256},
                                {"cmd": "stores", "addr": 517766,
                                 "count": 128},
                                {"cmd": "stores", "addr": 2615596,
                                 "count": 128},
                                {"cmd": "stores", "addr": 2615600,
                                 "count": 128},
                                {"cmd": "stores", "addr": 2615610,
                                 "count": 128},
                                {"cmd": "stores", "addr": 2615614,
                                 "count": 128}):
                    print(command["cmd"].upper() + " " + json.dumps(
                        request(args.port, command), sort_keys=True))
                report_dma_payloads(args.port, Path(args.disc))
                report_ram_modules(args.port)
                tail = request(args.port, {"cmd": "trace", "count": 128})
                if not args.compact:
                    print("FINAL_TRACE " + json.dumps([
                        {key: record[key] for key in
                         ("seq", "pc", "op", "sr", "d0", "d1", "d2", "d3",
                          "a0", "a1", "a2", "a7", "a7top", "frame")}
                        for record in tail["records"]
                    ], sort_keys=True))
                ikat = request(args.port, {"cmd": "ikat_events"})
                commands = [event for event in ikat["events"]
                            if event["type"] == 1]
                print("IKAT_COMMANDS " + json.dumps(
                    {"total": ikat["total"], "commands": commands},
                    sort_keys=True))
                malformed_seek = next(
                    (event for event in commands
                     if event["channel"] == 3 and
                     event["data"].startswith("E10001")), None)
                if args.compact and malformed_seek:
                    seek_trace = request(
                        args.port,
                        {"cmd": "trace",
                         "from": max(0, malformed_seek["trace_seq"] - 100),
                         "count": 100},
                    )
                    print("SEEK_TRACE " + json.dumps([
                        {key: record[key] for key in
                         ("seq", "pc", "op", "sr", "d0", "d1", "d2",
                          "d3", "a0", "a1", "a2", "a7", "a7top",
                          "frame")}
                        for record in seek_trace["records"]
                    ], sort_keys=True))
                if args.compact and commands:
                    rom_bytes = Path(args.rom).read_bytes()
                    trace_end = request(
                        args.port, {"cmd": "trace", "count": 1})["total"]
                    start = max(0, commands[-1]["trace_seq"] - 200000)
                    trap_records = []
                    while start < trace_end:
                        page = request(
                            args.port,
                            {"cmd": "trace", "from": start, "count": 256},
                        )
                        records = page["records"]
                        for record in records:
                            if record["op"] != 0x4E40:
                                continue
                            compact = {key: record[key] for key in
                                       ("seq", "pc", "sr", "d0", "d1",
                                        "d2", "d3", "a0", "a1", "a2",
                                        "a7top", "frame")}
                            offset = record["pc"] + 2 - 0x400000
                            if 0 <= offset <= len(rom_bytes) - 2:
                                compact["code"] = int.from_bytes(
                                    rom_bytes[offset:offset + 2], "big")
                            if compact.get("code") in range(0x07):
                                trap_records.append(compact)
                        if not records:
                            break
                        start = records[-1]["seq"] + 1
                    print("COMPACT_TRAPS " + json.dumps(trap_records,
                                                         sort_keys=True))
                    load_trap = next(
                        (record for record in reversed(trap_records)
                         if record.get("code") == 1), None)
                    if load_trap:
                        resume_pc = load_trap["pc"] + 4
                        scan = load_trap["seq"] + 1
                        resume_seq = None
                        while scan < trace_end and resume_seq is None:
                            page = request(
                                args.port,
                                {"cmd": "trace", "from": scan,
                                 "count": 256},
                            )
                            records = page["records"]
                            for record in records:
                                if record["pc"] == resume_pc:
                                    resume_seq = record["seq"]
                                    break
                            if not records:
                                break
                            scan = records[-1]["seq"] + 1
                        if resume_seq is not None:
                            load_return = request(
                                args.port,
                                {"cmd": "trace",
                                 "from": max(0, resume_seq - 16),
                                 "count": 64},
                            )
                            print("FLOAD_RETURN " + json.dumps([
                                {key: record[key] for key in
                                 ("seq", "pc", "op", "sr", "d0", "d1",
                                  "d2", "d3", "a0", "a1", "a2", "a7",
                                  "a7top", "frame")}
                                for record in load_return["records"]
                            ], sort_keys=True))
                c5 = next((event for event in reversed(commands)
                           if event["channel"] == 3 and
                           event["data"].startswith("C5")), None)
                if c5 and not args.compact:
                    around_c5 = request(
                        args.port, {"cmd": "trace",
                                    "from": max(0, c5["trace_seq"] - 32),
                                    "count": 256})
                    compact_trace = [
                        {key: record[key] for key in
                         ("seq", "pc", "op", "sr", "d0", "d1", "d2",
                          "a0", "a1", "a7", "a7top", "frame")}
                        for record in around_c5["records"]]
                    if not args.compact:
                        print("C5_TRACE " + json.dumps(compact_trace,
                                                        sort_keys=True))
                    trap_records = []
                    rom_bytes = Path(args.rom).read_bytes()
                    trace_end = request(
                        args.port, {"cmd": "trace", "count": 1})["total"]
                    start = max(0, c5["trace_seq"] - 100000)
                    end = min(trace_end, c5["trace_seq"] + 300000)
                    while start < end:
                        count = min(256, end - start)
                        page = request(
                            args.port,
                            {"cmd": "trace", "from": start, "count": count},
                        )
                        records = page["records"]
                        for record in records:
                            if record["op"] != 0x4E40:
                                continue
                            compact = {key: record[key] for key in
                                       ("seq", "pc", "sr", "d0", "d1",
                                        "d2", "d3", "a0", "a1", "a2",
                                        "a7top", "frame")}
                            offset = record["pc"] + 2 - 0x400000
                            if 0 <= offset <= len(rom_bytes) - 2:
                                compact["code"] = int.from_bytes(
                                    rom_bytes[offset:offset + 2], "big")
                            if compact.get("code") in range(0x07):
                                trap_records.append(compact)
                        if not records:
                            break
                        start = records[-1]["seq"] + 1
                    print("C5_TRAPS " + json.dumps(trap_records,
                                                   sort_keys=True))
                    post_c5 = [event for event in ikat["events"]
                               if event["seq"] > c5["seq"]]
                    if args.compact and post_c5:
                        anchor = post_c5[-1]
                        response_trace = request(
                            args.port,
                            {"cmd": "trace",
                             "from": max(0, anchor["trace_seq"] - 32),
                             "count": 256},
                        )
                        if response_trace["records"]:
                            response_trace["records"].extend(request(
                                args.port,
                                {"cmd": "trace",
                                 "from": response_trace["records"][-1]["seq"] + 1,
                                 "count": 256},
                            )["records"])
                        print("C5_RESPONSE " + json.dumps(
                            {"events": post_c5, "trace": [
                                {key: record[key] for key in
                                 ("seq", "pc", "op", "sr", "d0", "d1",
                                  "d2", "a0", "a1", "a7", "a7top",
                                  "frame")}
                                for record in response_trace["records"]
                                if (0x4277EA <= record["pc"] < 0x42C798 or
                                    0x4339FA <= record["pc"] < 0x434F36 or
                                    record["op"] == 0x4E40)
                            ]}, sort_keys=True))
                if args.compact:
                    ciap = ciap_register_events(args.port)
                    compact_ciap = [
                        {key: event[key] for key in
                         ("seq", "trace_seq", "frame", "pc", "offset",
                          "value", "size", "write")}
                        for event in ciap["register_events"]
                        if event["frame"] >= 1000 and event["offset"] in
                        (0x2584, 0x2586, 0x2594, 0x2596, 0x25C2, 0x3FFE)
                    ]
                    print("COMPACT_CIAP " + json.dumps(compact_ciap,
                                                        sort_keys=True))
                    if args.screenshot:
                        save_ppm(args.port, args.screenshot)
                    return 0
                ciap = ciap_register_events(args.port)
                histogram = Counter((event["offset"], event["write"],
                                     event["value"])
                                    for event in ciap["register_events"])
                common = [{"offset": key[0], "write": key[1],
                           "value": key[2], "count": count}
                          for key, count in histogram.most_common(40)]
                print("CIAP_REGISTER_SUMMARY " + json.dumps(
                    {"total": ciap["total"], "common": common,
                     "delivered_lbas": [
                         event["value"] for event in ciap["register_events"]
                         if event["offset"] == 0x3FFE]},
                    sort_keys=True))
                selection_offsets = {
                    0x2584, 0x2586, 0x258C, 0x258E, 0x2590,
                    0x2592, 0x2594, 0x2596, 0x25C0, 0x25C2, 0x3FFE,
                }
                selection_events = [
                    {key: event[key] for key in
                     ("seq", "trace_seq", "frame", "pc", "offset",
                      "value", "size", "write")}
                    for event in ciap["register_events"]
                    if event["offset"] in selection_offsets
                ]
                print("CIAP_SELECTION_EVENTS " + json.dumps(
                    selection_events, sort_keys=True))
                transactions = []
                markers = [index for index, event in enumerate(selection_events)
                           if event["offset"] == 0x3FFE]
                for marker_number, index in enumerate(markers):
                    end = (markers[marker_number + 1]
                           if marker_number + 1 < len(markers)
                           else len(selection_events))
                    event = selection_events[index]
                    transactions.append({
                        "lba": event["value"],
                        "frame": event["frame"],
                        "events": [
                            {key: following[key] for key in
                             ("seq", "offset", "value", "write")}
                            for following in selection_events[index + 1:end]
                            if following["offset"] in
                            (0x2584, 0x2586, 0x2594, 0x2596, 0x25C2)
                        ],
                    })
                print("CIAP_DELIVERY_TRANSACTIONS " + json.dumps(
                    transactions, sort_keys=True))
                delivery_traces = []
                for event in [event for event in ciap["register_events"]
                              if event["offset"] == 0x3FFE][:2]:
                    trace = request(
                        args.port,
                        {"cmd": "trace", "from": event["trace_seq"],
                         "count": 96},
                    )
                    delivery_traces.append({
                        "lba": event["value"],
                        "trace_seq": event["trace_seq"],
                        "records": [
                            {key: record[key] for key in
                             ("seq", "pc", "op", "sr", "d0", "d1",
                              "d2", "d3", "a0", "a1", "a2", "a7",
                              "a7top", "frame")}
                            for record in trace["records"]
                        ],
                    })
                print("CIAP_DELIVERY_TRACES " + json.dumps(
                    delivery_traces, sort_keys=True))
                if args.screenshot:
                    save_ppm(args.port, args.screenshot)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            stdout, stderr = proc.communicate()
            print("RUNTIME STDOUT\n" + stdout)
            print("RUNTIME STDERR\n" + stderr)
            print(f"RUNTIME EXIT {proc.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
