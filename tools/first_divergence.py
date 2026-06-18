#!/usr/bin/env python3
"""first_divergence.py — find the first PC where native and oracle diverge.

DEBUG.md THE LOOP, step 4: walk both execution rings from the start and report
the FIRST instruction where the recompiled runtime (CdiRuntime, :4380) and the
oracle (CdiOracle/CeDImu, :4381) part ways. Later differences are consequences;
this one is the bug.

Both expose the same trace surface (TCP.md): `trace {from:<seq>,count:N}` returns
per-instruction {seq,pc,sr,a7}, index-aligned because both capture one entry per
executed instruction. We page from seq 0 on each side and compare PC by PC.

Usage:
    # start both held first, e.g.:
    #   CdiRuntime bios\\cdi490a.rom --hold
    #   CdiOracle  bios\\cdi490a.rom --steps 100000 --hold
    python tools/first_divergence.py
    python tools/first_divergence.py --native 4380 --oracle 4381 --context 6
"""
import argparse, json, socket, sys

# The servers answer into an 8 KB buffer, so a page returns however many records
# fit (~150), not necessarily `count`. The reader below advances by the actual
# number returned, so the exact cap doesn't matter.
CHUNK = 128


def query(port, obj):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.split(b"\n", 1)[0].decode())


def trace_page(port, frm, count):
    return query(port, {"cmd": "trace", "from": frm, "count": count}).get("records", [])


def total(port):
    return query(port, {"cmd": "status", "blocks": 0})["blocks"]


class Reader:
    """Forward-only paged reader over a trace ring; tolerates short pages."""
    def __init__(self, port):
        self.port = port
        self.buf = []        # records not yet consumed
        self.next = 0        # seq to request next

    def get(self, seq):
        # records arrive in order; drop anything before `seq`, page in as needed
        while True:
            self.buf = [r for r in self.buf if r["seq"] >= seq]
            if self.buf and self.buf[0]["seq"] == seq:
                return self.buf[0]
            page = trace_page(self.port, max(seq, self.next), CHUNK)
            if not page:
                return None
            self.next = page[-1]["seq"] + 1
            self.buf = page
            if page[0]["seq"] > seq:   # requested seq already evicted
                return None


def fmt(r):
    return f"seq {r['seq']:>7}  PC=${r['pc']:08X}  SR=${r['sr']:04X}  A7=${r['a7']:08X}" if r else "(end of trace)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--native", type=int, default=4380)
    ap.add_argument("--oracle", type=int, default=4381)
    ap.add_argument("--context", type=int, default=8, help="instructions of context each side of the divergence")
    args = ap.parse_args()

    try:
        nt, ot = total(args.native), total(args.oracle)
    except OSError as e:
        print(f"cannot reach a server ({e}). Start both with --hold first.", file=sys.stderr)
        return 2

    n = min(nt, ot)
    print(f"native :{args.native} captured {nt} insns; oracle :{args.oracle} captured {ot}. "
          f"comparing {n} common.")

    nrd, ord_ = Reader(args.native), Reader(args.oracle)
    history = []   # recent (seq, native, oracle) for context

    for seq in range(n):
        nr, orr = nrd.get(seq), ord_.get(seq)
        if nr is None or orr is None:
            print(f"\ntrace truncated at seq {seq} (evicted from a ring); ran out of history.")
            return 0
        history.append((seq, nr, orr))
        if len(history) > args.context + 1:
            history.pop(0)
        if nr["pc"] != orr["pc"]:
            print(f"\n*** FIRST DIVERGENCE at seq {seq} ***")
            print(f"  native PC=${nr['pc']:08X}   oracle PC=${orr['pc']:08X}")
            print(f"\n  --- native (:{args.native}) ---")
            for s, hn, _ in history:
                print(("  > " if s == seq else "    ") + fmt(hn))
            print(f"\n  --- oracle (:{args.oracle}) ---")
            for s, _, ho in history:
                print(("  > " if s == seq else "    ") + fmt(ho))
            print("\nClassify per DEBUG.md: discovery/codegen, runtime/timing, memory/bus, "
                  "OS-9 HLE, or device. Trace the writer of the wrong edge.")
            return 1

    print(f"\nno PC divergence across {n} common instructions.")
    if nt < ot:
        print(f"native trace ENDS first ({nt} < {ot}) — the recompiled path runs out where "
              f"the oracle keeps going (dispatch miss / clean return). Check dispatch_miss_info.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
