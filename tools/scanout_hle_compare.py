#!/usr/bin/env python3
"""Compare built LLE/HLE Hotel Mario scanout jobs using a fixed field script.

Runs headlessly with emulated audio and isolated deterministic startup. Keeps
raw process CPU/field samples, complete RAM, output/device/CPU contracts and
frame history. It never writes guest progress or native saves. Timing compares
the gameplay interval separately from startup. Windows requires psutil.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import statistics
import subprocess
import time

import psutil
from bios_options_smoke import request
from build_provenance import verify
from hotelmario_controller_probe import snapshot, memory
from hotelmario_launch_probe import disc_bin_path, save_ppm
from collect_module_seeds import collect as collect_module_targets

SCRIPT = ("1000:16,1018:0,6000:16,6018:0,6250:16,6270:0,7400:16,7420:0,"
          "8000:4,8120:0,8200:16,8220:0,8400:32,8420:0,"
          "9600:4,9780:0,9850:16,9870:0,10000:32,10020:0,"
          "10400:1,10580:0,11000:4,11180:0,11200:16,11220:0")


def identity(path: Path) -> dict:
    return {"path": str(path.resolve()), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def run(exe: Path, args, label: str) -> dict:
    output = args.output / label
    output.mkdir()
    environment = os.environ.copy()
    environment.update(CDI_PACE_SPEED=str(args.speed), SDL_AUDIODRIVER="dummy",
                       TEMP=str(args.output), TMP=str(args.output))
    if args.presentation == "dummy":
        environment.update(SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software",
                           SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS="0")
    command = [str(exe.resolve()), str(args.rom.resolve()), "--disc",
               str(args.disc.resolve()), "--port", str(args.port),
               "--fault-hold",
               "--input-script", SCRIPT]
    command += (["--stop-seq", str(args.stop_seq)] if args.stop_seq else
                ["--stop-frame", str(args.end)])
    if args.presentation == "headless":
        command.append("--headless")
    samples = []
    host_before = psutil.cpu_times()
    began = time.monotonic()
    with (output / "stdout.log").open("w") as stdout, \
            (output / "stderr.log").open("w") as stderr, \
            (output / "samples.jsonl").open("w") as log:
        proc = subprocess.Popen(command, env=environment, stdout=stdout, stderr=stderr,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        try:
            process = psutil.Process(proc.pid)
            # A trace hold sleeps on the CPU thread; its later TCP capture CPU
            # cost is excluded from the timed endpoint below.
            deadline = time.monotonic() + args.timeout
            status = {}
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise RuntimeError(f"{label}: runtime exited {proc.returncode}")
                try:
                    status = request(args.port, {"cmd": "status"}, timeout=1)
                except OSError:
                    time.sleep(.05)
                    continue
                if status["miss_count"] or status.get("main_resets"):
                    raise RuntimeError(f"{label}: guest failure {status}")
                cpu = process.cpu_times()
                sample = {"wall_s": time.monotonic() - began, "status": status,
                          "cpu_user_s": cpu.user, "cpu_system_s": cpu.system,
                          "rss_bytes": process.memory_info().rss}
                samples.append(sample)
                log.write(json.dumps(sample) + "\n")
                log.flush()
                if status.get("held"):
                    if status["frame"] != args.end:
                        raise RuntimeError(f"{label}: unexpected hold {status}")
                    break
                time.sleep(.05)
            else:
                raise RuntimeError(f"{label}: deadline before field {args.end}: {status}")
            host_after = psutil.cpu_times()
            evidence = {name: request(args.port, {"cmd": name}) for name in (
                "status", "get_registers", "dispatch_miss_info", "ciap_state",
                "video_frame", "video_state", "audio_state", "native_state",
                "emu_ikat_state", "ikat_events", "native_events", "interp_report")}
            evidence["gameplay_at_end"] = snapshot(args.port, status)
            evidence["module_targets"] = collect_module_targets(args.port)
            # The permanent frame ring covers the complete deterministic run.
            # Drain it after the timed endpoint, not through expensive video
            # queries while the runtime is being measured.
            frames = []
            cursor = 1
            while cursor <= args.end:
                page = request(args.port, {"cmd": "frame_hashes", "from": cursor,
                                          "count": 256})["records"]
                if not page or page[0]["frame"] != cursor:
                    raise RuntimeError(f"incomplete frame history at {cursor}")
                frames.extend(page)
                cursor = page[-1]["frame"] + 1
            if len(frames) != args.end:
                raise RuntimeError("frame history has gaps")
            evidence["frame_hashes"] = frames
            # Fixed full instruction tail and complete RAM are stronger than
            # matching just the final visible image.
            tail = request(args.port, {"cmd": "trace", "count": 0})
            records = []
            for cursor in range(max(0, tail["total"] - 256), tail["total"], 32):
                records += request(args.port, {"cmd": "trace", "from": cursor,
                                                "count": 32})["records"]
            evidence["trace"] = {"total": tail["total"], "records": records}
            ram_hashes = []
            for base in (0, 0x200000):
                data = memory(args.port, base, 0x80000)
                (output / f"ram-{base:06x}.bin").write_bytes(data)
                ram_hashes.append(hashlib.sha256(data).hexdigest())
            (output / "evidence.json").write_text(json.dumps(evidence, indent=2))
            save_ppm(args.port, output / "frame.ppm")
            if evidence["gameplay_at_end"] is None:
                raise RuntimeError(f"{label}: workload did not establish active gameplay")
            # Interpolate cumulative CPU time at the exact start field. The
            # retained bracketing samples disclose the sub-sample uncertainty.
            upper = next(i for i, s in enumerate(samples) if s["status"]["frame"] >= args.start)
            if not upper:
                raise RuntimeError("missing pre-gameplay timing bracket")
            before, after = samples[upper - 1], samples[upper]
            fraction = (args.start - before["status"]["frame"]) / (
                after["status"]["frame"] - before["status"]["frame"])
            def cumulative(sample):
                return sample["cpu_user_s"] + sample["cpu_system_s"]
            cpu_start = cumulative(before) + fraction * (cumulative(after) - cumulative(before))
            wall_start = before["wall_s"] + fraction * (after["wall_s"] - before["wall_s"])
            cpu_seconds = cumulative(samples[-1]) - cpu_start
            wall_seconds = samples[-1]["wall_s"] - wall_start
            result = {"label": label, "runtime": identity(exe), "command": command,
                      "start_field": args.start, "end_field": args.end,
                      "cpu_s": cpu_seconds, "wall_s": wall_seconds,
                      "cpu_ms_per_field": 1000 * cpu_seconds / (args.end - args.start),
                      "fields_per_s": (args.end - args.start) / wall_seconds,
                      "peak_sampled_rss_bytes": max(s["rss_bytes"] for s in samples),
                      "start_bracket": [before, after], "ram_sha256": ram_hashes,
                      "host_cpu_before": host_before._asdict(),
                      "host_cpu_after": host_after._asdict()}
            busy = (host_after.user - host_before.user) + (host_after.system - host_before.system)
            idle = host_after.idle - host_before.idle
            result["host_average_cpu_busy_percent"] = 100 * busy / (busy + idle)
            (output / "result.json").write_text(json.dumps(result, indent=2))
            print(json.dumps({key: result[key] for key in (
                "label", "cpu_ms_per_field", "fields_per_s", "ram_sha256")}), flush=True)
            return result
        finally:
            if proc.poll() is None:
                proc.terminate()
            proc.wait(timeout=10)


def contract(output: Path, a: str, b: str, presentation: str,
             cpu_implementations_differ: bool = False) -> dict:
    left = json.loads((output / a / "evidence.json").read_text())
    right = json.loads((output / b / "evidence.json").read_text())
    # Only native implementation counters can differ. Host audio queue/drop
    # counts remain recorded; headless has no consumer, so even those should
    # match in this workload.
    for state in (left, right):
        for key in ("scanout_jobs", "fused_jobs"):
            state["video_state"].pop(key, None)
        if presentation == "dummy":
            # SDL consumes PCM on host time. Only its queue/drop counters can
            # differ; the entire decoded PCM hash and guest transport remain
            # part of the exact contract.
            for key in ("queued_frames", "dropped_frames"):
                state["audio_state"].pop(key, None)
        if cpu_implementations_differ:
            # These describe which host implementation executed an instruction,
            # not guest state. Keep total trace sequence/cycles, the complete
            # register tail, all device events/output and RAM under comparison.
            for key in ("insns", "interp"):
                state["status"].pop(key, None)
            state["native_state"].pop("dispatches", None)
            # Native binding is implementation bookkeeping. Its event may be
            # recorded before/after the tier's pending cycle flush. Retain the
            # image/base/type/epoch and trace sequence, excluding only that
            # implementation timestamp. IKAT hardware event timing stays exact;
            # its PC attribution can name the just-completed or next opcode.
            for event in state["native_events"]["events"]:
                event.pop("cycles", None)
            for event in state["ikat_events"]["events"]:
                event.pop("pc", None)
            state.pop("interp_report", None)
            state.pop("module_targets", None)
    differences = [key for key in left if left[key] != right[key]]
    for base in (0, 0x200000):
        name = f"ram-{base:06x}.bin"
        if (output / a / name).read_bytes() != (output / b / name).read_bytes():
            differences.append(name)
    result = {"left": a, "right": b, "equal": not differences,
              "differences": differences}
    if differences:
        raise RuntimeError(f"contract mismatch: {result}")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("rom", type=Path)
    parser.add_argument("disc", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--port", type=int, default=4468)
    parser.add_argument("--start", type=int, default=9400)
    parser.add_argument("--end", type=int, default=13000)
    parser.add_argument("--pairs", type=int, default=3)
    parser.add_argument("--speed", type=int, default=32)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--presentation", choices=("headless", "dummy"), default="headless")
    parser.add_argument("--package", default="native scanout spans + exact frame publication/content reuse")
    parser.add_argument("--reference-package", default="reference LLE scanout and original native module catalog")
    parser.add_argument("--cpu-implementations-differ", action="store_true")
    parser.add_argument("--stop-seq", type=int,
                        help="common instruction-entry hold immediately after end field")
    args = parser.parse_args()
    if not 0 < args.start < args.end or not 1 <= args.pairs <= 10:
        parser.error("require 0 < start < end and 1..10 pairs")
    if not 1 <= args.speed <= 32:
        parser.error("speed must be in 1..32")
    with socket.socket() as probe:
        probe.settimeout(.2)
        if probe.connect_ex(("127.0.0.1", args.port)) == 0:
            parser.error(f"port {args.port} already has a listener")
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    provenance = {"reference": verify(args.reference), "candidate": verify(args.candidate)}
    identity_data = {"schema": 1, "game": "Hotel Mario (USA)",
                     "optimization_package": args.package,
                     "reference_package": args.reference_package,
                     "cpu_implementations_differ": args.cpu_implementations_differ,
                     "stop_seq": args.stop_seq,
                     "platform": platform.platform(), "cpu_count": os.cpu_count(),
                     "rom": identity(args.rom), "disc": identity(disc_bin_path(args.disc)),
                     "provenance": provenance, "script": SCRIPT,
                     "source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                     "core_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root / "external/m68k-recomp-core", text=True).strip(),
                     "core_changes": subprocess.check_output(["git", "diff"], cwd=root / "external/m68k-recomp-core", text=True),
                     "game_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root.parent / "HotelMarioRecomp", text=True).strip(),
                     "source_diff": subprocess.check_output(["git", "diff"], cwd=root, text=True),
                     "harness": identity(Path(__file__)),
                     "presentation": ("headless; canonical software scanout; no host presentation"
                         if args.presentation == "headless" else
                         "SDL dummy video/software renderer; no desktop window; actual frontend copy/upload/present path"),
                     "audio": ("emulated and hashed; no headless PCM consumer"
                         if args.presentation == "headless" else "emulated and hashed; SDL dummy PCM output"),
                     "start_method": "deterministic CLI field input; no host-timed clicks or guest writes"}
    (args.output / "identity.json").write_text(json.dumps(identity_data, indent=2))
    shutil.copyfile(Path(__file__), args.output / "harness.py")
    # Warm each binary before alternating the retained timed pairs.
    run(args.reference, args, "warm-reference")
    run(args.candidate, args, "warm-candidate")
    results = []
    contracts = [contract(args.output, "warm-reference", "warm-candidate", args.presentation,
                          args.cpu_implementations_differ)]
    for pair in range(args.pairs):
        order = (("reference", args.reference), ("candidate", args.candidate))
        if pair & 1:
            order = tuple(reversed(order))
        for backend, exe in order:
            results.append(run(exe, args, f"pair{pair + 1}-{backend}"))
        contracts.append(contract(args.output, f"pair{pair + 1}-reference", f"pair{pair + 1}-candidate",
                                  args.presentation, args.cpu_implementations_differ))
    a = [s["cpu_ms_per_field"] for s in results if s["label"].endswith("-reference")]
    b = [s["cpu_ms_per_field"] for s in results if s["label"].endswith("-candidate")]
    savings = [100 * (1 - h / l) for l, h in zip(a, b)]
    summary = {"schema": 1, "game": "Hotel Mario (USA)",
               "optimization_package": identity_data["optimization_package"],
               "reference_package": identity_data["reference_package"],
               "samples": results, "contracts": contracts,
               "reference_cpu_ms_per_field": a, "candidate_cpu_ms_per_field": b,
               "paired_cpu_savings_percent": savings,
               "median_cpu_savings_percent": statistics.median(savings),
               "status": "measured improvement" if min(savings) > 3 else "review individual samples",
               "scope": "one built title, fixed early-gameplay interval; no broader title/platform/release claim"}
    (args.output / "campaign-evidence.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps({key: summary[key] for key in (
        "game", "optimization_package", "reference_cpu_ms_per_field", "candidate_cpu_ms_per_field",
        "paired_cpu_savings_percent", "median_cpu_savings_percent", "status")}, indent=2))


if __name__ == "__main__":
    main()
