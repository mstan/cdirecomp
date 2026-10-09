#!/usr/bin/env python3
"""Check complete USA Hotel Mario attract circuits from retained evidence.

The original intro_sub increments gds+0x7C6 and wraps at 10, selecting demos
1..9. cdi_L0_av.map supplies eight records per scene; each scene's first
record bounds its extent in L0_av.rtf. This gate requires a return to demo 1
after every nine-scene circuit, rather than inferring completion from time.
It never launches a game or changes guest state.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

from hotelmario_scenario import FATAL_PC, FATAL_VIDEO_HASHES

USA_DISC_SHA256 = "31cf7c28f4e7e7dcf731633bfcb4a5ae9459794aecfcd3d765df66d2442722f9"
MAP_LBA, MAP_BYTES, AV_LBA, AV_SECTORS = 27416, 1280, 28250, 2657


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise ValueError(reason)


def scene_starts(disc: Path) -> list[int]:
    # The supplied image is checked independently of the scenario metadata.
    with disc.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
        require(digest == USA_DISC_SHA256, "attract layout requires the verified USA disc")
        stream.seek(MAP_LBA * 2352)
        sector = stream.read(2352)
    require(len(sector) == 2352 and sector[15] == 2 and sector[16:20] == sector[20:24]
            and not sector[18] & 0x20, "AV map is not a valid Mode-2 Form-1 sector")
    data = sector[24:24 + MAP_BYTES]
    require(data[:2] == b"\x4a\xfc" and int.from_bytes(data[4:8], "big") == MAP_BYTES
            and int.from_bytes(data[0x30:0x34], "big") == 0x44
            and int.from_bytes(data[0x44:0x46], "big") == 80,
            "original AV map header/layout differs")
    starts = [AV_LBA + struct.unpack_from(">I", data, 0x4C + scene * 8 * 12)[0]
              for scene in range(10)]
    require(all(a < b for a, b in zip(starts, starts[1:]))
            and starts[-1] < AV_LBA + AV_SECTORS, "invalid scene extents")
    return starts


def check(directory: Path, cycles: int, require_audio: bool) -> dict:
    metadata = json.loads((directory / "identity.json").read_text())
    result = json.loads((directory / "result.json").read_text())
    require(result.get("ok"), f"scenario failed: {result.get('reason')}")
    evidence = json.loads((directory / "evidence.json").read_text())
    require(metadata.get("speed") == 1 and not metadata.get("input_script"),
            "attract acceptance requires normal pacing and no input script")
    require(metadata.get("build_provenance", {}).get("runtime_sha256") ==
            metadata["runtime"]["sha256"], "linked binary provenance is missing")
    require(metadata["disc"]["sha256"] == USA_DISC_SHA256, "wrong scenario disc")
    require(evidence.get("module_targets", {}).get("ok") and
            not evidence["module_targets"].get("dropped"), "coverage capture is incomplete")
    starts = scene_starts(Path(metadata["disc"]["path"]))
    transitions = []
    last_scene = None
    last_field = -1
    baseline_resets = None
    with (directory / "samples.jsonl").open() as stream:
        for line in stream:
            sample = json.loads(line)
            status, video = sample["status"], sample["video"]
            field = status["frame"]
            require(field >= last_field, "guest field counter went backwards")
            last_field = field
            if baseline_resets is None:
                baseline_resets = status["main_resets"]
            require(status["main_resets"] == baseline_resets and not status["miss_count"]
                    and not status["held"] and status["pc"] != FATAL_PC
                    and video["argb_fnv1a"] not in FATAL_VIDEO_HASHES,
                    f"guest failure at field {field}")
            require(sample["native"]["compiled"] == 174, "missing compiled module identities")
            if transitions:
                require(not status["input"], f"controller input during attract at field {field}")
            lba = sample["ciap"]["drive_lba"]
            if starts[1] <= lba < AV_LBA + AV_SECTORS:
                scene = max(index for index, start in enumerate(starts) if start <= lba)
                if scene != last_scene:
                    require(scene == (1 if last_scene in (None, 9) else last_scene + 1),
                            f"demo order changed from {last_scene} to {scene} at field {field}")
                    transitions.append({"field": field, "demo": scene, "lba": lba})
                    last_scene = scene
    returns = [item["field"] for item in transitions if item["demo"] == 1]
    complete = max(0, len(returns) - 1)
    require(complete >= cycles, f"only {complete}/{cycles} complete attract circuits")
    final = evidence["status"]
    require(not final["miss_count"] and final["main_resets"] == baseline_resets
            and final["pc"] != FATAL_PC, "final guest state failed")
    if require_audio:
        audio = evidence["audio_state"]
        require(metadata["windowed"] and metadata["audio_driver"] != "dummy"
                and audio["sample_frames"] > 0 and audio["dropped_frames"] == 0,
                "audible SDL output requires decoded PCM with zero drops")
    return {"ok": True, "complete_circuits": complete, "required_circuits": cycles,
            "circuit_start_fields": returns, "scene_transitions": transitions,
            "runtime_sha256": metadata["runtime"]["sha256"],
            "disc_sha256": USA_DISC_SHA256, "audio_output_checked": require_audio,
            "scope": "input-free attract circuits; no campaign or listening-quality claim"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", type=Path)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--require-audio-output", action="store_true")
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error("cycles must be positive")
    try:
        result = check(args.scenario, args.cycles, args.require_audio_output)
    except (OSError, ValueError, KeyError) as error:
        result = {"ok": False, "reason": str(error)}
    (args.scenario / "attract-gate.json").write_text(json.dumps(result, indent=2))
    print(json.dumps({key: value for key, value in result.items() if key != "scene_transitions"}))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
