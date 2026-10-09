#!/usr/bin/env python3
"""Drive a running USA Hotel Mario gameplay probe through real controller input.

This development observer reads RAM and uses set_input, which the original
OS-9 driver consumes through timed IKAT packets. It never changes guest memory,
forces stages, patches the game or substitutes game logic. A completed probe
is not a campaign certificate. Run hotelmario_scenario.py separately to retain
asset/build identities, device traffic and final fault evidence.

The layout comes from the original USA cdi_hotel module and its shipped STB:
init_doors counts 0x2000 tiles as open; 0x1000 tiles are closed. the_game advances
when gds+0x52A reaches zero. The actor collision boxes come from mario_coll_det.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

from bios_options_smoke import request

LEFT, UP, RIGHT, DOWN, JUMP, DOOR = 1, 2, 4, 8, 16, 32
DOOR_X = [44, 88, 132, 176, 220, 264, 308]
PLAYER_FUNCTION_OFFSET = 0x17710
MAIN_MODULE_BYTES = 112984


def u16(data: bytes, offset: int = 0) -> int:
    return int.from_bytes(data[offset:offset + 2], "big")


def u32(data: bytes, offset: int = 0) -> int:
    return int.from_bytes(data[offset:offset + 4], "big")


def memory(port: int, address: int, size: int) -> bytes:
    chunks = []
    for offset in range(0, size, 4096):
        count = min(4096, size - offset)
        response = request(port, {"cmd": "read_mem", "addr": address + offset, "len": count})
        chunk = bytes.fromhex(response["bytes"])
        if len(chunk) != count:
            raise RuntimeError("incomplete RAM observation")
        chunks.append(chunk)
    return b"".join(chunks)


def snapshot(port: int, status: dict) -> dict | None:
    gds = u32(memory(port, 0x4310, 4))
    if not 0 < gds < 0x7F000:
        return None
    data = memory(port, gds, 0x8CA)
    player = u16(data, 0x596)
    sprite_address = u32(data, 0x250)
    if player > 1 or not 0x200000 <= sprite_address <= 0x27FE80:
        return None
    sprite = memory(port, sprite_address, 0x180)
    function = u32(sprite)
    base = function - PLAYER_FUNCTION_OFFSET
    if not 0x200000 <= base <= 0x280000 - MAIN_MODULE_BYTES:
        return None
    header = memory(port, base, 8)
    if u16(header) != 0x4AFC or u32(header, 4) != MAIN_MODULE_BYTES:
        return None
    players = [[u16(data, 0x7A0 + index * 0x1C + offset)
                for offset in range(0, 0x1C, 2)] for index in range(2)]
    progress = players[player]
    if not 1 <= progress[3] <= 8 or progress[4] > 15 or progress[5] > 99:
        return None
    result = {"frame": status["frame"], "pc": status["pc"], "gds": gds,
              "sprite": sprite_address, "player_function": function,
              "x": u16(sprite, 0x10E), "y": u16(sprite, 0x110),
              "floor": u16(sprite, 0x112), "state": u16(sprite, 0x11A),
              "substate": u16(sprite, 0x11E), "death": u16(data, 0x53A),
              "facing": u16(sprite, 0x122), "movement_type": u16(sprite, 0x12E),
              "jump_block": u16(sprite, 0x13A),
              "xs": [u16(data, 0x4A6 + i * 2) for i in range(7)],
              "progress": progress, "players": players, "player": player,
              "two_players": u16(data, 0x598),
              "door_total": u16(data, 0x528), "door_open": u16(data, 0x52A)}
    board = u32(data, 0x244)
    objects = u32(data, 0x248)
    if not (0x200000 <= board < 0x27FF80 and 0x200000 <= objects < 0x27FFC0):
        return None
    tiles = memory(port, board + 0x12, 70)
    result["tiles"] = [u16(tiles, i * 2) for i in range(35)]
    count = u16(memory(port, objects, 2))
    if count > 64 or objects + 0x34 + count * 0x274 > 0x280000:
        return None
    pool = memory(port, objects + 0x34, count * 0x274)
    enemies = []
    for index in range(count):
        actor = pool[index * 0x274:index * 0x274 + 0x50]
        if u16(actor, 0x26) == 3 and not u16(actor, 0x4E) and u16(actor, 4) >= 10:
            enemies.append({"id": index, "type": u16(actor, 4), "floor": u16(actor, 0xE),
                            "left": u16(actor, 0x1A), "right": u16(actor, 0x1C),
                            "top": u16(actor, 0x1E), "bottom": u16(actor, 0x20),
                            "secondary": [u16(actor, j) for j in range(0x3C, 0x46, 2)]})
    result["enemies"] = enemies
    result["observed_through_field"] = request(port, {"cmd": "status"})["frame"]
    return result


class Controller:
    def __init__(self):
        self.ride_start = None
        self.jump_direction = None
        self.jump_enemy = None
        self.target = None

    def steer(self, record: dict) -> int:
        field, x, floor = record["frame"], record["x"], record["floor"]
        state, substate = record["state"], record["substate"]
        raw, enemies, xs = record["tiles"], record["enemies"], record["xs"]
        if not (0 <= floor < 5 and 0 <= x <= 360 and xs == DOOR_X and not record["death"]):
            return JUMP if field % 180 < 25 else 0
        opened = [i for i in range(7) if raw[floor * 7 + i] & 0xF000 == 0x2000]
        remaining = [i for i in range(35) if raw[i] & 0xF000 == 0x2000]
        wanted = min((i // 7 for i in remaining), default=floor)
        mask = 0
        if state in (1, 2):
            if substate == 0:
                arrived = self.ride_start is None or floor != self.ride_start
                # Coming out takes several animation fields. Stay in the
                # elevator until that exit corridor is clear of walking actors.
                blocked = any(e["floor"] == floor and e["right"] > x - 55 and
                              e["left"] < x + 80 for e in enemies)
                mask = (0 if blocked else DOWN) if arrived else (JUMP if field % 36 < 18 else 0)
        elif state == 4:
            blocked = any(e["floor"] == floor and e["right"] > x - 55 and
                          e["left"] < x + 80 for e in enemies)
            mask = 0 if blocked else DOWN
        elif opened and floor == wanted:
            self.ride_start = None
            self.target = min(opened, key=lambda i: abs(xs[i] - x))
            delta = xs[self.target] - x
            mask = RIGHT if delta > 8 else LEFT if delta < -8 else (DOOR if field % 18 < 9 else 0)
        else:
            if not remaining:
                return JUMP if field % 60 < 20 else 0
            elevators = [i for i in range(7) if raw[floor * 7 + i] & 0xF000 in (0xE000, 0xF000)]
            preferred = [i for i in elevators if raw[floor * 7 + i] & 0xF000 == (0xF000 if wanted < floor else 0xE000)]
            if preferred or elevators:
                self.target = min(preferred or elevators, key=lambda i: abs(xs[i] - x))
                delta = xs[self.target] - x
                mask = RIGHT if delta > 8 else LEFT if delta < -8 else UP
                self.ride_start = floor if mask == UP else None
            else:
                mask = JUMP
        if state == 0:
            self.jump_direction = None
            self.jump_enemy = None
            direction = 1 if mask & RIGHT else -1 if mask & LEFT else 0
            def threat(enemy):
                if enemy["floor"] != floor:
                    return False
                if direction > 0:
                    return x + 7 < enemy["right"] and enemy["left"] - (x + 25) < 42
                if direction < 0:
                    return enemy["left"] < x + 25 and x + 7 - enemy["right"] < 42
                return enemy["right"] > x and enemy["left"] < x + 35
            threats = [enemy for enemy in enemies if threat(enemy)] if mask not in (UP, DOOR) else []
            if threats:
                above = [e for e in enemies if e["floor"] == floor - 1 and
                         e["right"] > x - (145 if direction < 0 else 35) and
                         e["left"] < x + (150 if direction > 0 else 65)]
                if above:
                    nearest = min(threats, key=lambda e: abs((e["left"] + e["right"]) / 2 - (x + 16)))
                    distance = nearest["left"] - (x + 25) if direction >= 0 else x + 7 - nearest["right"]
                    directly_above = any(e["right"] > x - 20 and e["left"] < x + 55 for e in above)
                    if distance < 30 and not directly_above:
                        mask = JUMP
                        self.jump_direction = 0
                        self.jump_enemy = nearest["id"]
                    elif distance < 30:
                        mask = LEFT if direction >= 0 else RIGHT
                        if x < 44 and mask == LEFT or x > 308 and mask == RIGHT:
                            mask = 0
                    else:
                        mask = 0
                else:
                    mask = (mask & (LEFT | RIGHT)) | JUMP
                    self.jump_direction = mask & (LEFT | RIGHT)
                    self.jump_enemy = min(threats, key=lambda e: abs((e["left"] + e["right"]) / 2 - (x + 16)))["id"]
        elif state == 3:
            # jump_rise preserves the takeoff facing (+0x122). For running
            # jumps, an opposite direction sets +0x13A and stops horizontal
            # travel; neutral input continues it. It cannot reverse in midair.
            travel = LEFT if record["facing"] & 1 else RIGHT
            brake = RIGHT if travel == LEFT else LEFT
            overhead = [e for e in enemies if e["floor"] == floor - 1 and e["right"] > x - 20 and e["left"] < x + 55]
            horizontal = travel if record["movement_type"] == 1 else 0
            if overhead or x < 32 and travel == LEFT or x > 312 and travel == RIGHT:
                horizontal = brake
            elif substate >= 2:
                landing = next((e for e in enemies if e["id"] == self.jump_enemy and e["floor"] == floor), None)
                if landing:
                    delta = (landing["left"] + landing["right"]) / 2 - (x + 16)
                    ahead = delta if travel == RIGHT else -delta
                    if ahead < 28:
                        horizontal = brake
            mask = horizontal | (JUMP if substate < 3 and not overhead else 0)
        if state == 0 and (x < 32 and mask & LEFT or x > 328 and mask & RIGHT):
            mask = RIGHT if x < 32 else LEFT
        return mask


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=3600)
    parser.add_argument("--minimum-field", type=int, default=9400)
    args = parser.parse_args()
    if args.seconds <= 0 or args.minimum_field < 0:
        parser.error("duration must be positive; minimum field cannot be negative")
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "controller-runs.jsonl").open("a") as runs:
        runs.write(json.dumps({"started_unix_seconds": time.time(), "argv": sys.argv,
                               "source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}) + "\n")
    controller = Controller()
    deadline = time.monotonic() + args.seconds
    previous = None
    next_devices = 0
    reason = "observation duration completed"
    fault = False
    try:
        with (args.output / "controller.jsonl").open("a") as log, \
                (args.output / "controller-devices.jsonl").open("a") as devices:
            while time.monotonic() < deadline:
                status = request(args.port, {"cmd": "status"})
                if status["held"] or status["miss_count"] or status["pc"] == 0x260466:
                    fault = bool(status["miss_count"] or status["pc"] == 0x260466)
                    reason = "guest fault" if fault else "guest held; inspect scenario capture"
                    break
                if time.monotonic() >= next_devices:
                    observed = {"host_monotonic_ns": time.monotonic_ns(),
                                "host_unix_seconds": time.time(), "status": status}
                    for command in ("ciap_state", "native_state", "audio_state", "video_frame"):
                        observed[command] = request(args.port, {"cmd": command})
                    observed["module_targets"] = request(args.port, {"cmd": "module_targets", "count": 0})
                    devices.write(json.dumps(observed) + "\n")
                    devices.flush()
                    next_devices = time.monotonic() + 1
                if status["frame"] < args.minimum_field:
                    time.sleep(0.03)
                    continue
                record = snapshot(args.port, status)
                if record is None:
                    request(args.port, {"cmd": "set_input", "mask": 0})
                    time.sleep(0.03)
                    continue
                record["stale"] = record["observed_through_field"] - record["frame"] > 12
                mask = 0 if record["stale"] else controller.steer(record)
                record["mask"] = mask
                record["target_column"] = controller.target
                request(args.port, {"cmd": "set_input", "mask": mask})
                log.write(json.dumps(record) + "\n")
                log.flush()
                milestone = (record["player"], tuple(record["progress"]), record["state"],
                             record["floor"], record["door_open"])
                if milestone != previous:
                    print(json.dumps(record), flush=True)
                    previous = milestone
                time.sleep(0.03)
    finally:
        try:
            request(args.port, {"cmd": "set_input", "mask": 0})
        except (OSError, ValueError):
            pass
    print(json.dumps({"reason": reason, "fault": fault, "campaign_certificate": False}), flush=True)
    return 1 if fault else 0


if __name__ == "__main__":
    raise SystemExit(main())
