#!/usr/bin/env python3
"""Run a single input-driven Hotel Mario scenario and retain failure evidence.

No launch retries, guest-memory edits, or forced completions. Accelerated runs
are investigation only; --speed 1 is the player pacing acceptance profile.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import socket
from pathlib import Path
import subprocess
import time

from bios_options_smoke import click_with_frontend, request, wait_shell
from cdi_click import click, move_to
from hotelmario_launch_probe import disc_bin_path, save_ppm
from collect_module_seeds import collect as collect_module_targets
from build_provenance import verify as verify_build

FATAL_VIDEO_HASHES = {
    "eeead5f1888ae323": "Hotel Mario reported a disc read failure",
    "30499f92593aaa23": "Hotel Mario reported a disc read failure",
}
# The verified USA cdi_hotel load has hm_error's terminal BRA -2 here. The
# framebuffer changes with the active CLUT, so a single screen hash is not a
# sufficient failure detector. Confirm the original self-loop before failing.
FATAL_PC = 0x260466


def guest_failure(port: int, status: dict, video: dict) -> str | None:
    if status.get("pc") == FATAL_PC:
        instruction = request(port, {"cmd": "read_mem", "addr": FATAL_PC, "len": 2})
        if instruction.get("bytes", "").lower() == "60fe":
            return "Hotel Mario entered hm_error's terminal loop"
    return FATAL_VIDEO_HASHES.get(video.get("argb_fnv1a"))


def identity(path: Path) -> dict:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"path": str(path.resolve()), "bytes": path.stat().st_size,
            "sha256": digest.hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", type=Path)
    parser.add_argument("rom", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--port", type=int, default=4430)
    parser.add_argument("--speed", type=float, default=1)
    parser.add_argument("--seconds", type=float, default=180)
    parser.add_argument("--stall-seconds", type=float, default=15)
    parser.add_argument("--stop-lba", type=int,
                        help="capture after this observed drive position")
    parser.add_argument("--stop-frame", type=int,
                        help="capture after this guest field")
    parser.add_argument("--stop-video-hash", help="capture after this observed completed framebuffer hash")
    parser.add_argument("--stop-pc", type=lambda value: int(value, 0),
                        help="capture at this already-recorded instruction PC")
    parser.add_argument("--stop-pc-skip", type=int, default=0)
    parser.add_argument("--input-script",
                        help="runtime guest-field input schedule: frame:mask,...")
    parser.add_argument("--watch-store", action="append", type=lambda value: int(value, 0),
                        default=[], help="retain writers covering this address from the always-on ring")
    parser.add_argument("--trace-tail", type=int, default=512,
                        help="instruction records retained at final capture")
    parser.add_argument("--generated-dir", type=Path,
                        help="additional generated module sources linked into this runtime")
    parser.add_argument("--windowed", action="store_true")
    parser.add_argument("--require-build-provenance", action="store_true",
                        help="reject binaries whose link-time inputs changed")
    parser.add_argument("--audio-driver", default="dummy",
                        help="SDL audio driver (dummy for investigation; e.g. directsound for listening)")
    args = parser.parse_args()
    if args.speed <= 0 or args.seconds <= 0 or args.stall_seconds <= 0:
        parser.error("speed and durations must be positive")
    if not 1 <= args.trace_tail <= 1048576:
        parser.error("trace-tail must fit the instruction ring (1..1048576)")
    with socket.socket() as probe:
        probe.settimeout(0.2)
        if probe.connect_ex(("127.0.0.1", args.port)) == 0:
            parser.error(f"port {args.port} already has a listener")
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    metadata = {"runtime": identity(args.runtime), "rom": identity(args.rom),
                "disc": identity(disc_bin_path(args.disc)),
                "speed": args.speed, "windowed": args.windowed,
                "input_script": args.input_script, "seconds": args.seconds,
                "stall_seconds": args.stall_seconds,
                "stop_lba": args.stop_lba, "stop_frame": args.stop_frame,
                "stop_video_hash": args.stop_video_hash,
                "stop_pc": args.stop_pc, "stop_pc_skip": args.stop_pc_skip,
                "watch_store": args.watch_store,
                "trace_tail": args.trace_tail,
                "audio_driver": args.audio_driver,
                "source_revision": subprocess.check_output(
                    ["git", "rev-parse", "HEAD"], cwd=root, encoding="utf-8").strip(),
                "source_changes": subprocess.check_output(
                    ["git", "diff"], cwd=root, encoding="utf-8"),
                "core_revision": subprocess.check_output(
                    ["git", "rev-parse", "HEAD"], cwd=root / "external/m68k-recomp-core", encoding="utf-8").strip(),
                "core_changes": subprocess.check_output(
                    ["git", "diff"], cwd=root / "external/m68k-recomp-core", encoding="utf-8"),
                "generated": [identity(p) for p in
                              sorted((root / "bios/generated").glob("*.c"))],
                "scenario_tool": identity(Path(__file__))}
    if args.generated_dir:
        metadata["generated_modules"] = [identity(p) for p in sorted(args.generated_dir.glob("*.c"))]
    if args.require_build_provenance:
        metadata["build_provenance"] = verify_build(args.runtime)
    config = args.output.resolve() / "player.cfg"
    config.write_text("[input]\ncapture_mouse = false\n[rtc]\n"
                      "sync_host_on_startup = false\n", encoding="utf-8")
    environment = os.environ.copy()
    environment["CDI_PLAYER_CONFIG_PATH"] = str(config)
    environment["CDI_PACE_SPEED"] = str(args.speed)
    environment["SDL_AUDIODRIVER"] = args.audio_driver
    command = [str(args.runtime.resolve()), str(args.rom.resolve()), "--disc",
               str(args.disc.resolve()), "--port", str(args.port), "--fault-hold"]
    if not args.windowed:
        command.append("--headless")
    if args.input_script:
        command.extend(("--input-script", args.input_script))
    metadata["command"] = command
    (args.output / "identity.json").write_text(json.dumps(metadata, indent=2))
    result = {"ok": False, "reason": "incomplete"}
    proc = None
    with (args.output / "stdout.log").open("w") as stdout, \
            (args.output / "stderr.log").open("w") as stderr, \
            (args.output / "samples.jsonl").open("w") as samples, \
            (args.output / "ciap-registers.jsonl").open("w") as registers, \
            (args.output / "stores.jsonl").open("w") as stores:
        try:
            proc = subprocess.Popen(command, cwd=root, env=environment,
                                    stdout=stdout, stderr=stderr,
                                    creationflags=getattr(subprocess,
                                                          "CREATE_NO_WINDOW", 0))
            shell = wait_shell(proc, args.port, time.monotonic() + 45,
                               minimum_frame=1000)
            initial_resets = shell.get("main_resets", 0)
            if initial_resets:
                raise RuntimeError("guest restarted the main CPU before reaching the player shell")
            if args.stop_pc is not None:
                armed = request(args.port, {"cmd": "stop_pc", "pc": args.stop_pc,
                                           "skip": args.stop_pc_skip})
                if not armed.get("ok"):
                    raise RuntimeError(f"cannot arm PC stop: {armed}")
            if args.stop_frame is not None:
                armed = request(args.port, {"cmd": "stop_frame", "frame": args.stop_frame})
                if not armed.get("ok"):
                    raise RuntimeError(f"cannot arm field stop: {armed}")
            move_to("127.0.0.1", args.port, 630, 165)
            if args.windowed:
                click_with_frontend(args.port)
            else:
                click("127.0.0.1", args.port)
            print("Play CD-i selected", flush=True)
            deadline = time.monotonic() + args.seconds
            previous_lba = None
            progress_at = time.monotonic()
            previous = None
            event_cursor = 0
            store_cursors = dict.fromkeys(args.watch_store, 0)
            def drain_ledgers():
                nonlocal event_cursor
                tail = request(args.port, {"cmd": "ciap_events", "count": 1})
                if event_cursor < tail["oldest"]:
                    registers.write(json.dumps({"evicted_from": event_cursor,
                                                "evicted_to": tail["oldest"]}) + "\n")
                    event_cursor = tail["oldest"]
                while event_cursor < tail["total"]:
                    page = request(args.port, {"cmd": "ciap_events",
                                   "from": event_cursor, "count": 256})
                    events = page["events"]
                    if not events:
                        break
                    for event in events:
                        if 0x2580 <= event["offset"] < 0x2600:
                            registers.write(json.dumps(event) + "\n")
                    event_cursor = events[-1]["seq"] + 1
                registers.flush()
                for address, cursor in store_cursors.items():
                    while True:
                        page = request(args.port, {"cmd": "stores", "addr": address,
                                                   "from": cursor, "count": 64})
                        if cursor < page["oldest"]:
                            stores.write(json.dumps({"address": address,
                                                     "evicted_from": cursor,
                                                     "evicted_to": page["oldest"]}) + "\n")
                        for event in page["records"]:
                            stores.write(json.dumps({"address": address, **event}) + "\n")
                        next_cursor = page["next"]
                        if next_cursor <= cursor and cursor < page["total"]:
                            raise RuntimeError("store ledger did not advance")
                        cursor = next_cursor
                        if cursor >= page["total"]:
                            break
                    store_cursors[address] = cursor
                stores.flush()
            reason = "observation window completed"
            while time.monotonic() < deadline:
                status = request(args.port, {"cmd": "status"})
                if (args.stop_pc is not None and status.get("held") and
                        status["pc"] == args.stop_pc and not status["miss_count"]):
                    reason = f"captured instruction ${args.stop_pc:06X}"
                    break
                if (args.stop_frame is not None and status.get("held") and
                        status["frame"] == args.stop_frame and not status["miss_count"]):
                    reason = f"captured guest field {args.stop_frame}"
                    break
                if status["miss_count"] or status.get("held"):
                    raise RuntimeError(f"guest fault/dispatch miss: {status}")
                if proc.poll() is not None:
                    raise RuntimeError(f"runtime exited: {proc.returncode}")
                if status.get("main_resets", 0) != initial_resets:
                    raise RuntimeError("guest restarted the main CPU during the scenario")
                ciap = request(args.port, {"cmd": "ciap_state"})
                video = request(args.port, {"cmd": "video_frame"})
                sample = {"status": status, "ciap": ciap, "video": video,
                          "native": request(args.port, {"cmd": "native_state"})}
                samples.write(json.dumps(sample) + "\n")
                samples.flush()
                failure = guest_failure(args.port, status, video)
                if failure:
                    raise RuntimeError(failure)
                # Retain register traffic before the ring evicts it. Sector
                # payload reads stay in the bounded runtime ring.
                drain_ledgers()
                if args.stop_video_hash and video["argb_fnv1a"] == args.stop_video_hash:
                    reason = f"observed framebuffer {args.stop_video_hash}"
                    break
                summary = (ciap["drive_lba"], ciap["running"],
                           ciap["waiting_ack"], video["argb_fnv1a"])
                if summary != previous:
                    print(f"frame={status['frame']} lba={ciap['drive_lba']} "
                          f"run={ciap['running']} hold={ciap['waiting_ack']} "
                          f"video={video['argb_fnv1a']}", flush=True)
                    previous = summary
                if ciap["drive_lba"] != previous_lba:
                    previous_lba = ciap["drive_lba"]
                    progress_at = time.monotonic()
                if (ciap["running"] and ciap["waiting_ack"] and
                        time.monotonic() - progress_at > args.stall_seconds):
                    raise RuntimeError(f"CD transport stalled: {ciap}")
                if args.stop_lba is not None and ciap["drive_lba"] >= args.stop_lba:
                    reason = f"observed drive position {ciap['drive_lba']}"
                    break
                if args.stop_frame is not None and status["frame"] >= args.stop_frame:
                    break
                time.sleep(0.25)
            result = {"ok": True, "reason": reason,
                      "certification": "observation only; no gameplay claim"}
            if reason == "observation window completed" and any(
                    value is not None for value in (args.stop_frame, args.stop_pc, args.stop_lba, args.stop_video_hash)):
                raise RuntimeError("deadline expired before the requested capture point")
        except Exception as error:
            result = {"ok": False, "reason": str(error)}
            print(result["reason"], flush=True)
        finally:
            if proc is not None and proc.poll() is None:
                try:
                    request(args.port, {"cmd": "pause"})
                    time.sleep(0.1)
                    if "drain_ledgers" in locals():
                        drain_ledgers()
                    evidence = {}
                    for cmd in ("dispatch_miss_info", "status", "ciap_state",
                                "get_registers", "emu_ikat_state", "ikat_events",
                                "video_state", "audio_state", "interp_report", "native_state", "native_events"):
                        evidence[cmd] = request(args.port, {"cmd": cmd})
                    evidence["ciap_events"] = request(
                        args.port, {"cmd": "ciap_events", "count": 256})
                    tail = request(args.port, {"cmd": "trace", "count": 0})
                    end = tail["total"]
                    cursor = max(0, end - args.trace_tail)
                    records = []
                    # Full register records exceed the response capacity when
                    # requested as one large page; retain the actual tail.
                    while cursor < end:
                        page = request(args.port, {"cmd": "trace", "from": cursor,
                                                   "count": min(64, end - cursor)})
                        if not page["records"]:
                            raise RuntimeError("instruction trace did not advance")
                        records.extend(page["records"])
                        cursor = records[-1]["seq"] + 1
                    evidence["trace"] = {"ok": True, "total": end, "records": records}
                    # Older baseline binaries lack this newly added diagnostic.
                    # Retain their other evidence without treating an unsupported
                    # read-only query as a guest failure.
                    capability = request(args.port, {"cmd": "module_targets", "count": 0})
                    evidence["module_targets"] = (collect_module_targets(args.port)
                        if capability.get("ok") else capability)
                    evidence["video_frame"] = request(args.port, {"cmd": "video_frame"})
                    fatal = guest_failure(args.port, evidence["status"], evidence["video_frame"])
                    if fatal:
                        result = {"ok": False, "reason": fatal}
                    (args.output / "evidence.json").write_text(
                        json.dumps(evidence, indent=2))
                    save_ppm(args.port, args.output / "frame.ppm")
                    for base in (0, 0x200000):
                        memory = bytearray()
                        for offset in range(0, 0x80000, 4096):
                            page = request(args.port, {"cmd": "read_mem",
                                           "addr": base + offset, "len": 4096})
                            memory.extend(bytes.fromhex(page["bytes"]))
                        (args.output / f"ram-{base:06x}.bin").write_bytes(memory)
                except Exception as error:
                    result["capture_error"] = str(error)
                    result["ok"] = False
                    result["reason"] = f"evidence capture failed: {error}; scenario: {result['reason']}"
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            (args.output / "result.json").write_text(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
