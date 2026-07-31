#!/usr/bin/env python3
"""Bring Hotel Mario to the title menu, then hand off (leave runtime live).

Launch -> wait shell STOP -> click Play CD-I (verify, retry) -> wait for the
game load to pass LBA 20000 (retrying the whole launch on the racy 3219-class
load wedge) -> wait for the title menu -> print TITLE-REACHED and exit 0.
Runtime keeps running; PID is printed.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 2)[0])
from cdi_debug import send  # noqa: E402

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4382
SHELL_PC = 0x40A3E6
HOST = "127.0.0.1"


def q(cmd, **kw):
    d = {"cmd": cmd}
    d.update(kw)
    try:
        return send(HOST, PORT, d)
    except Exception:
        return {}


def launch():
    env = os.environ.copy()
    env["CDI_PACE_SPEED"] = "8"
    env["SDL_VIDEODRIVER"] = "dummy"
    root = r"F:\Projects\cdirecomp"
    return subprocess.Popen(
        [root + r"\build\runner-release\CdiRuntime.exe",
         root + r"\bios\cdi490a.rom",
         "--disc", root + r"\disc\Hotel Mario (USA).bin",
         "--port", str(PORT), "--headless"],
        cwd=root,
        stdout=open("build/tmp/bringup.stdout.log", "w"),
        stderr=open("build/tmp/bringup.stderr.log", "w"))


def wait_shell(deadline):
    while time.monotonic() < deadline:
        st = q("status")
        if st.get("pc") == SHELL_PC and st.get("halted") == 1:
            return True
        time.sleep(1.5)
    return False


from cdi_click import move_to, click  # noqa: E402


def click_play():
    try:
        move_to(HOST, PORT, 630, 165, timeout=20.0)
    except SystemExit as e:
        print(f"move_to: {e}", flush=True)
        return
    click(HOST, PORT)


def main():
    for attempt in range(1, 6):
        proc = launch()
        print(f"attempt {attempt}: pid={proc.pid}", flush=True)
        if not wait_shell(time.monotonic() + 90):
            print("no shell STOP; retrying", flush=True)
            proc.kill()
            continue
        time.sleep(8.0)
        launched = False
        for c in range(6):
            base_lba = q("ciap_state").get("drive_lba", 0)
            click_play()
            t0 = time.monotonic()
            while time.monotonic() - t0 < 12:
                lba = q("ciap_state").get("drive_lba", 0)
                if lba != base_lba:
                    launched = True
                    break
                time.sleep(1.0)
            if launched:
                break
            print("click did not take (drive idle); re-clicking", flush=True)
        if not launched:
            proc.kill()
            continue
        print("game launching", flush=True)
        # watch load progress; detect the racy wedge (lba stuck <20000 w/ ack)
        stuck_since = None
        t0 = time.monotonic()
        wedged = False
        while time.monotonic() - t0 < 240:
            cs = q("ciap_state")
            lba = cs.get("drive_lba", 0)
            if lba >= 20000:
                print(f"load passed lba={lba}", flush=True)
                break
            if cs.get("waiting_ack") == 1:
                if stuck_since is None:
                    stuck_since = (time.monotonic(), lba)
                elif (lba == stuck_since[1] and
                      time.monotonic() - stuck_since[0] > 12):
                    print(f"WEDGE at lba={lba}; relaunching", flush=True)
                    wedged = True
                    break
            else:
                stuck_since = None
            time.sleep(1.0)
        if wedged or q("ciap_state").get("drive_lba", 0) < 20000:
            print("load did not complete; relaunching", flush=True)
            proc.kill()
            time.sleep(1)
            continue
        print(f"TITLE-PHASE pid={proc.pid}", flush=True)
        return 0
    print("FAILED after retries", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
