#!/usr/bin/env python3
"""realign_divergence.py — find the TRUE divergence past benign wait-loop jitter.

first_divergence.py is index-aligned: it compares native[seq] vs oracle[seq] and
stops at the first mismatch. But the CD-i boot has benign frame-sync spin loops
(e.g. the MCD212 DA poll) where native and oracle execute a DIFFERENT NUMBER of
iterations, then re-converge. Index alignment trips on that jitter and reports it
as "the divergence", masking the real one downstream.

This tool re-aligns. It compares by FULL register state (pc,sr,d0-7,a0-7), and
when two records don't match it searches a lookahead window on BOTH sides for the
nearest re-sync (one side spun extra iterations of a loop). It only declares a
TRUE divergence when neither side reaches the other's state within the window —
i.e. a real control-flow / data split, not a spin-count difference.

Both runs must have the comparison window in their rings. Typical setup:
    CdiOracle  rom --steps 250000 --hold              # :4381, ring holds 0..250000
    CdiRuntime rom --stop-seq 250000 --port 4390      # :4390, ring holds 0..250000
    python tools/realign_divergence.py --native 4390 --oracle 4381

Tuning:
    --start N    first seq to load/compare (default 1; skips the seq-0 reset seam)
    --window W   lookahead for re-sync (default 4096); raise if a long spin loop
                 is falsely flagged as a divergence
    --context K  rows of context printed each side
"""
import argparse, json, socket, sys

REG_KEYS = ["pc", "sr"] + [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)]


class Trace:
    """Persistent-connection bulk reader: pulls [start, total) into memory once.

    One socket, many requests (the server keeps the connection open), so loading
    250k records is ~1k round-trips on a single connection instead of per-page
    reconnects."""
    def __init__(self, port):
        self.port = port
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.rbuf = b""
        self.seqs = []          # parallel arrays: seq[i] and state tuple[i]
        self.states = []

    def _rpc(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())
        while b"\n" not in self.rbuf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise IOError("server closed")
            self.rbuf += chunk
        line, self.rbuf = self.rbuf.split(b"\n", 1)
        return json.loads(line.decode())

    def total(self):
        return self._rpc({"cmd": "status", "blocks": 0})["blocks"]

    def load(self, start, stop):
        cur = start
        while cur < stop:
            recs = self._rpc({"cmd": "trace", "from": cur, "count": 256}).get("records", [])
            if not recs:
                break
            if recs[0]["seq"] > cur:        # requested window already evicted
                raise IOError(f"seq {cur} evicted from :{self.port} (oldest is {recs[0]['seq']})")
            for r in recs:
                if r["seq"] < cur:
                    continue
                self.seqs.append(r["seq"])
                self.states.append(tuple(r.get(k, 0) for k in REG_KEYS))
            cur = recs[-1]["seq"] + 1
            if cur >= stop:
                break

    def fmt(self, i):
        if i >= len(self.states):
            return "(end of trace)"
        st = self.states[i]
        d = dict(zip(REG_KEYS, st))
        ds = " ".join(f"D{k}={d['d'+str(k)]:08X}" for k in range(8))
        as_ = " ".join(f"A{k}={d['a'+str(k)]:08X}" for k in range(8))
        return f"seq {self.seqs[i]:>7} PC=${d['pc']:08X} SR=${d['sr']:04X}\n        {ds}\n        {as_}"


def first_diff_field(a, b):
    for idx, k in enumerate(REG_KEYS):
        if a[idx] != b[idx]:
            return k
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--native", type=int, default=4390)
    ap.add_argument("--oracle", type=int, default=4381)
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--window", type=int, default=4096)
    ap.add_argument("--context", type=int, default=6)
    args = ap.parse_args()

    try:
        N, O = Trace(args.native), Trace(args.oracle)
        nt, ot = N.total(), O.total()
        stop = min(nt, ot)
        print(f"native :{args.native} has {nt}; oracle :{args.oracle} has {ot}. "
              f"loading [{args.start}, {stop}) into memory ...", flush=True)
        N.load(args.start, stop)
        O.load(args.start, stop)
    except (OSError, IOError) as e:
        print(f"load failed: {e}. Start both held with the window in their rings.", file=sys.stderr)
        return 2
    print(f"loaded native={len(N.states)} oracle={len(O.states)} records. re-aligning ...", flush=True)

    W = args.window
    SUBST_MAX = 16   # max run of substituted (mismatched-then-realigned) records
                     # to tolerate: a capture-timing seam at a trap is 1-2 records
                     # (native reflects the exception frame push one seq earlier
                     # than the oracle), while a REAL control-flow split never
                     # re-aligns. Kept small so it can't mask a genuine divergence.
    i = j = 0
    resyncs = 0
    max_skew = 0
    while i < len(N.states) and j < len(O.states):
        if N.states[i] == O.states[j]:
            i += 1; j += 1
            continue
        # Search for the nearest re-sync, smallest distance first. Three moves:
        #   both-advance (substitution): one side's record has no exact twin
        #     (a trap capture seam) but the streams re-align d records later;
        #   oracle-ahead / native-ahead (insertion/deletion): one side spun extra
        #     iterations of a wait loop.
        hit = None
        for d in range(1, W + 1):
            if d <= SUBST_MAX and i + d < len(N.states) and j + d < len(O.states) \
                    and N.states[i + d] == O.states[j + d]:
                hit = ("both", d); break
            if j + d < len(O.states) and O.states[j + d] == N.states[i]:
                hit = ("oracle", d); break
            if i + d < len(N.states) and N.states[i + d] == O.states[j]:
                hit = ("native", d); break
        if hit is None:
            field = first_diff_field(N.states[i], O.states[j])
            print(f"\n*** TRUE DIVERGENCE — native seq {N.seqs[i]} vs oracle seq {O.seqs[j]} "
                  f"(register {field.upper()}) ***")
            print(f"  no re-sync within {W} records either side: a real control-flow/data split.")
            print(f"  ({resyncs} benign re-syncs skipped earlier; max spin skew {max_skew}).")
            print(f"\n  --- native (:{args.native}) ---")
            for k in range(max(0, i - args.context), min(len(N.states), i + 2)):
                print(("  > " if k == i else "    ") + N.fmt(k))
            print(f"\n  --- oracle (:{args.oracle}) ---")
            for k in range(max(0, j - args.context), min(len(O.states), j + 2)):
                print(("  > " if k == j else "    ") + O.fmt(k))
            print(f"\nThe last MATCHING state is the row before each '>'. The instruction that ran "
                  f"there branched/wrote differently. Classify per DEBUG.md: codegen, memory/bus, "
                  f"or device/interrupt (an event the oracle received and the native did not).")
            return 1
        side, d = hit
        resyncs += 1
        max_skew = max(max_skew, d)
        if side == "both":
            i += d; j += d   # capture seam: d substituted records, then re-aligned
        elif side == "oracle":
            j += d           # oracle had d extra records (spun more); skip to the match
        else:
            i += d           # native had d extra records; skip to the match
    print(f"\nno TRUE divergence across the loaded window "
          f"({resyncs} benign re-syncs, max spin skew {max_skew}). "
          f"native ran {len(N.states)}, oracle {len(O.states)}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
