#!/usr/bin/env python3
"""Full attract verification: wait for shell, click Play, then sample
frame/argb-hash/drive_lba every 2s for the whole attract sequence."""
import json, socket, sys, time

HOST, PORT = "127.0.0.1", 4382
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 150.0


def send(obj, timeout=5):
    with socket.create_connection((HOST, PORT), timeout=timeout) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.split(b"\n", 1)[0].decode(errors="replace"))


def try_send(obj):
    try:
        return send(obj)
    except Exception:
        return None


# wait for server + shell idle
t0 = time.monotonic()
while time.monotonic() - t0 < 90:
    st = try_send({"cmd": "status"})
    if st and st.get("halted") == 1 and st.get("frame", 0) > 500:
        break
    time.sleep(2)
else:
    print("shell never became idle", flush=True)
    sys.exit(1)
print(f"shell idle at frame {st['frame']}; clicking Play", flush=True)
send({"cmd": "set_input", "mask": 0x10})
time.sleep(0.3)
send({"cmd": "set_input", "mask": 0})

t0 = time.monotonic()
last_hash = None
while time.monotonic() - t0 < DUR:
    st = try_send({"cmd": "status"})
    cs = try_send({"cmd": "ciap_state"})
    fh = try_send({"cmd": "frame_hashes", "count": 1})
    h = None
    if fh:
        recs = fh.get("records") or fh.get("hashes") or []
        if recs:
            h = recs[-1].get("argb_fnv1a")
    changed = "*" if h != last_hash else " "
    last_hash = h
    err = try_send({"cmd": "read_mem", "addr": 0x400C, "len": 4})
    print(f"t={time.monotonic()-t0:5.1f}s frame={st and st.get('frame')} "
          f"lba={cs and cs.get('drive_lba')} run={cs and cs.get('running')} "
          f"ack={cs and cs.get('waiting_ack')} miss={st and st.get('miss_count')} "
          f"err={err and err.get('bytes')} hash={h}{changed}", flush=True)
    time.sleep(2)
print("done", flush=True)
