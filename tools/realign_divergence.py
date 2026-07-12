#!/usr/bin/env python3
"""realign_divergence.py — find the TRUE divergence past benign wait-loop jitter.

first_divergence.py is index-aligned: it compares native[seq] vs oracle[seq] and
stops at the first mismatch. But the CD-i boot has benign frame-sync spin loops
(e.g. the MCD212 DA poll) where native and oracle execute a DIFFERENT NUMBER of
iterations, then re-converge. Index alignment trips on that jitter and reports it
as "the divergence", masking the real one downstream.

This tool re-aligns. It compares by FULL register state (pc,sr,d0-7,a0-7) PLUS
the stack top [A7] and the two INACTIVE stack-pointer shadows (usp,ssp — see
COSIM-SPEC.md §2a; whichever of the pair isn't currently aliased by A7 was
previously invisible, the SSP-frontier blind spot) (so a context-switch / caller
split with identical registers but a different pushed return address, OR an
inactive-stack-pointer split, can't hide behind register alignment), and when
two records don't match it searches a lookahead window on BOTH sides for the
nearest re-sync (one side spun extra iterations of a loop). It only declares a
TRUE divergence when neither side reaches the other's state within the window —
i.e. a real control-flow / data split, not a spin-count difference.

Once two records ARE register-aligned at the same seq, it also checks the two
guest-RAM incremental hashes (ram0_h/ram1_h, COSIM-SPEC.md §2b/§4). These are
NOT folded into the alignment key itself (a spin loop legitimately mutates RAM
between iterations and must still align on registers alone) — they are a
divergence CHECK applied once alignment holds, the same way the writing-loop
heuristic below checks address-register marches after a skip re-syncs. A
register-identical, ram-hash-different pair is the "same registers, different
memory" divergence class the register-only tool was blind to.

It also un-masks WRITING-LOOP divergences: a one-sided skip is benign only if it
is an in-place poll/wait loop (no pointer advances). If the skipped records march
an address register (a fill/copy loop), a different iteration count means a
different span of memory was written — a real data split that register-only
re-alignment papers over (both sides re-sync to equal registers at loop exit).
Requires a7top capture on both rings (debug_server.c / cdi_oracle.cpp).

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

REG_KEYS = (["pc", "sr"] + [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)]
            + ["usp", "ssp", "a7top"])
A_REG_IDX = {n: REG_KEYS.index(f"a{n}") for n in range(8)}   # state-tuple index of An
A7TOP_IDX = REG_KEYS.index("a7top")
A7_IDX    = REG_KEYS.index("a7")
USP_IDX   = REG_KEYS.index("usp")
SSP_IDX   = REG_KEYS.index("ssp")
# Non-stack fields: everything except A7, a7top, and SSP. Two states are
# "seam-equal" if they agree on all of these — used to bridge an exception-frame
# CAPTURE seam (a take where one side's handler-entry record already reflects the
# pushed frame in A7/a7top and the other's does not yet), which is a 1-record
# trace artifact, not a divergence. See the seam bridge in the re-align loop.
# SSP is excluded here (not just A7/a7top) because a "take" seam only exists
# mid-exception: SR is already required to match exactly (it's IN this set), so
# both candidate records are already proven to be in supervisor mode (S=1) —
# and per the canonical form (COSIM-SPEC.md §2a) canonical_ssp == A7 whenever
# S=1. SSP is therefore A7's alias here, not an independent field: it MUST
# move by the identical delta, checked explicitly in seam_eq below (never
# tolerated on its own — a real SSP-only divergence, e.g. the SSP-frontier
# class, still falls straight through to a TRUE divergence).
NONSTACK_IDX = [k for k in range(len(REG_KEYS)) if k not in (A7_IDX, A7TOP_IDX, SSP_IDX)]

def seam_eq(a, b):
    """True if a and b agree on every field except A7, a7top, and SSP (an
    exception-frame capture seam): A7 must differ by a plausible SCC68070 frame
    size (short = 6 or 8 bytes; long bus/addr-error = 34), and SSP must differ
    by that SAME delta (it canonically aliases A7 while supervisor — see the
    NONSTACK_IDX comment above) — so a genuine stack divergence (an arbitrary
    A7 delta, or an SSP delta that does not track A7) is NOT masked."""
    if any(a[k] != b[k] for k in NONSTACK_IDX):
        return False
    a7_delta = a[A7_IDX] - b[A7_IDX]
    if abs(a7_delta) not in (6, 8, 34):
        return False
    return (a[SSP_IDX] - b[SSP_IDX]) == a7_delta


def addr_advances(states, rng):
    """Writing-loop signature: across the records in `rng` (a one-sided skip that
    re-aligned), does some address register A0..A6 march monotonically?

    A benign wait/poll loop re-reads the SAME location every iteration — no
    pointer advances (only a down-counter Dn, or nothing, changes). A fill/copy
    loop ADVANCES a pointer each iteration (MOVE.L D1,(A2)+ ; DBF), touching a
    span of memory. So a one-sided skip whose An marches is NOT benign jitter —
    it is a different number of WRITES, a real divergence the register-only
    realign would otherwise paper over (both sides re-sync to identical regs at
    the loop exit, hiding that different memory was written).

    Returns (reg_n, first_val, last_val, stride) on a hit, else None. A7 is
    excluded — stack push/pop is not a fill — and so is the captured a7top."""
    idxs = list(rng)
    if len(idxs) < 2:
        return None            # need >=2 records to see a march
    for n in range(7):         # A0..A6
        ai = A_REG_IDX[n]
        vals = [states[k][ai] for k in idxs]
        diffs = [vals[t + 1] - vals[t] for t in range(len(vals) - 1)]
        nz = [x for x in diffs if x != 0]
        if len(nz) >= 2 and (all(x > 0 for x in nz) or all(x < 0 for x in nz)):
            return n, vals[0], vals[-1], nz[0]
    return None


def parse_ram_hash(v):
    """Parse a ram0_h/ram1_h wire value: a 16-char lowercase hex STRING
    (COSIM-SPEC.md §4), consumed as int(x, 16). Tolerates absence (missing key
    -> None from .get) or a literal 0 for backward compat with traces recorded
    before these fields existed — both parse to 0, which the divergence check
    below then treats as "no signal from this side" rather than a false split
    against a real hash."""
    if not v:
        return 0
    if isinstance(v, str):
        return int(v, 16)
    return int(v)


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
        self.ram0h = []         # parsed ram0_h per record (int; 0 = absent/old trace)
        self.ram1h = []         # parsed ram1_h per record (int; 0 = absent/old trace)

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
                self.ram0h.append(parse_ram_hash(r.get("ram0_h")))
                self.ram1h.append(parse_ram_hash(r.get("ram1_h")))
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
        ram = ""
        if self.ram0h[i] or self.ram1h[i]:
            ram = f"\n        ram0_h={self.ram0h[i]:016x} ram1_h={self.ram1h[i]:016x}"
        return (f"seq {self.seqs[i]:>7} PC=${d['pc']:08X} SR=${d['sr']:04X} "
                f"[A7]=${d['a7top']:08X} USP=${d['usp']:08X} SSP=${d['ssp']:08X}\n"
                f"        {ds}\n        {as_}{ram}")


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
    ap.add_argument("--loop-window", type=int, default=400,
                    help="how far to look on the OTHER side for a skipped PC before "
                         "deeming it a real control-flow split (vs a benign wait-loop iteration)")
    ap.add_argument("--log-skips", type=int, default=0, metavar="D",
                    help="print every one-sided re-sync of >= D records (0 = off)")
    ap.add_argument("--no-split-detect", action="store_true",
                    help="don't stop on a control-flow split OR a writing-loop count "
                         "divergence; only stop on a same-position register mismatch "
                         "(the old behaviour)")
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
            # Registers (incl. usp/ssp) are aligned at this seq — now check the
            # signal register-alignment is blind to: the two guest-RAM
            # incremental hashes (COSIM-SPEC.md §2b/§4). ram*_h is deliberately
            # NOT part of the state tuple above (a spin loop legitimately
            # mutates RAM between iterations and must still align on registers
            # alone) — this is a divergence CHECK applied once alignment holds,
            # mirroring how addr_advances() below checks writing-loop spans
            # after a skip re-syncs, rather than folding into the LCS key.
            # 0 means "absent/old trace on this side" (backward compat) and is
            # never compared, so it can't produce a false split.
            ram_split = []
            if N.ram0h[i] and O.ram0h[j] and N.ram0h[i] != O.ram0h[j]:
                ram_split.append(("ram0_h", N.ram0h[i], O.ram0h[j]))
            if N.ram1h[i] and O.ram1h[j] and N.ram1h[i] != O.ram1h[j]:
                ram_split.append(("ram1_h", N.ram1h[i], O.ram1h[j]))
            if ram_split:
                names = ", ".join(b for b, _, _ in ram_split)
                print(f"\n*** MEMORY DIVERGENCE — native seq {N.seqs[i]} vs oracle seq {O.seqs[j]} "
                      f"(ram0/ram1 hash differs (same registers)) ***")
                print(f"  registers (incl. usp/ssp) are byte-identical at this seq, but {names} "
                      f"differ: a real guest-RAM divergence invisible to the register-only realign "
                      f"(COSIM-SPEC.md guest-RAM section).")
                for b, nh, oh in ram_split:
                    print(f"    {b}: native={nh:016x}  oracle={oh:016x}")
                print(f"  ({resyncs} benign re-syncs skipped earlier; max spin skew {max_skew}).")
                print(f"\n  --- native (:{args.native}) ---")
                for k in range(max(0, i - args.context), min(len(N.states), i + 2)):
                    print(("  > " if k == i else "    ") + N.fmt(k))
                print(f"\n  --- oracle (:{args.oracle}) ---")
                for k in range(max(0, j - args.context), min(len(O.states), j + 2)):
                    print(("  > " if k == j else "    ") + O.fmt(k))
                print(f"\n  The instruction that ran at the last row wrote memory differently on each "
                      f"side even though its own register effects agree. Drill: re-run each side to "
                      f"--stop-seq {N.seqs[i]}, read_mem the {names.replace('_h', '')} bank region on "
                      f"both sides, diff bytes.")
                return 1
            i += 1; j += 1
            continue
        # Search for the nearest re-sync, smallest distance first. Three moves:
        #   both-advance (substitution): one side's record has no exact twin
        #     (a trap capture seam) but the streams re-align d records later;
        #   oracle-ahead / native-ahead (insertion/deletion): one side spun extra
        #     iterations of a wait loop.
        hit = None
        for d in range(1, W + 1):
            # both-advance (substitution) is a benign capture SEAM only if every
            # substituted record is the SAME instruction on both sides (same PC,
            # differing only in a register snapshot — e.g. a trap frame push the
            # native reflects one seq earlier than the oracle). If the PCs differ
            # it is real divergent code (e.g. JSR to a shared callee from two
            # different sites: the streams converge at the callee but the call
            # sites — and the pushed return addresses — differ), so DON'T mask it.
            if d <= SUBST_MAX and i + d < len(N.states) and j + d < len(O.states) \
                    and N.states[i + d] == O.states[j + d] \
                    and all(N.states[i + k][0] == O.states[j + k][0] for k in range(d)):
                hit = ("both", d); break
            if j + d < len(O.states) and O.states[j + d] == N.states[i]:
                hit = ("oracle", d); break
            if i + d < len(N.states) and N.states[i + d] == O.states[j]:
                hit = ("native", d); break
        # Exception-frame capture-seam bridge. A take (interrupt/trap) under a
        # wait-loop skew leaves the re-sync TARGET differing ONLY in A7/a7top:
        # one side's handler-entry record already shows the pushed frame, the
        # other captured the pre-push registers. The exact search above rejects
        # it; accept it iff the record immediately AFTER the seam exact-aligns
        # (so it is provably a 1-record artifact, not a control-flow split).
        seam = False
        if hit is None:
            for d in range(0, W + 1):
                # oracle spun d extra records, then took (seam at O[j+d] ~ N[i])
                if j + d < len(O.states) and seam_eq(O.states[j + d], N.states[i]) \
                        and i + 1 < len(N.states) and j + d + 1 < len(O.states) \
                        and N.states[i + 1] == O.states[j + d + 1]:
                    hit = ("oracle", d); seam = True; break
                # native spun d extra records, then took (seam at N[i+d] ~ O[j])
                if i + d < len(N.states) and seam_eq(N.states[i + d], O.states[j]) \
                        and j + 1 < len(O.states) and i + d + 1 < len(N.states) \
                        and O.states[j + 1] == N.states[i + d + 1]:
                    hit = ("native", d); seam = True; break
        if seam:
            side, d = hit
            resyncs += 1
            max_skew = max(max_skew, d)
            if args.log_skips:
                who = "oracle" if side == "oracle" else "native"
                print(f"  [exception-frame seam bridged @ {who} seq "
                      f"{(O.seqs[j+d] if side=='oracle' else N.seqs[i+d])} "
                      f"PC=${N.states[i][0]:06X}; {who} spun {d} extra record(s) then took]")
            if side == "oracle":
                i += 1; j += d + 1
            else:
                i += d + 1; j += 1
            continue
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
        # A one-sided skip means one machine executed `d` records the other did
        # not. That is benign ONLY if it is extra iterations of a wait loop BOTH
        # run — i.e. the skipped PCs also appear in the other side's stream
        # nearby. If a skipped PC is NOT seen on the other side within a window,
        # one machine ran code the other never does: a real CONTROL-FLOW split
        # (a different branch / caller) that register-alignment would otherwise
        # paper over (a shared callee re-normalizes the registers downstream).
        if side in ("oracle", "native") and d >= 1:
            if side == "oracle":
                src_states, rng = O.states, range(j, j + d)
                skipped = [O.states[k][0] for k in rng]
                lo = max(0, i - args.loop_window)
                seen = {N.states[k][0] for k in range(lo, min(len(N.states), i + args.loop_window))}
                seqs_a, seqs_b = (O.seqs[j], "oracle"), (N.seqs[i], "native")
            else:
                src_states, rng = N.states, range(i, i + d)
                skipped = [N.states[k][0] for k in rng]
                lo = max(0, j - args.loop_window)
                seen = {O.states[k][0] for k in range(lo, min(len(O.states), j + args.loop_window))}
                seqs_a, seqs_b = (N.seqs[i], "native"), (O.seqs[j], "oracle")
            foreign = [pc for pc in skipped if pc not in seen]
            if args.log_skips and d >= args.log_skips:
                pcs = sorted(set(skipped))
                tag = f"  !! {len(foreign)} FOREIGN PCs" if foreign else ""
                print(f"  skip {side} d={d} at {seqs_b[1]} seq {seqs_b[0]} "
                      f"(${N.states[i][0] if side=='native' else O.states[j][0]:06X}); "
                      f"skipped {len(pcs)} distinct: "
                      f"{', '.join('$%06X'%p for p in pcs[:8])}{' …' if len(pcs)>8 else ''}{tag}")
            if foreign and not args.no_split_detect:
                k = next(idx for idx in (range(j, j + d) if side == "oracle" else range(i, i + d))
                         if (O.states if side == "oracle" else N.states)[idx][0] == foreign[0])
                print(f"\n*** CONTROL-FLOW SPLIT — {side} ran code the other side does not ***")
                print(f"  at {seqs_a[1]} seq {seqs_a[0]} the {side} took a branch into "
                      f"${foreign[0]:06X} that {seqs_b[1]} never executes near here.")
                print(f"  (register-alignment masked this: a shared callee re-normalizes the "
                      f"registers, so the streams look equal again downstream — this is the "
                      f"earlier root split behind any later same-register/different-memory divergence.)")
                print(f"  {resyncs} benign re-syncs skipped earlier; max spin skew {max_skew}.")
                src = O if side == "oracle" else N
                oth_i = i if side == "oracle" else j
                oth = N if side == "oracle" else O
                print(f"\n  --- {side} (ran the extra code) ---")
                for kk in range(max(0, (j if side=='oracle' else i) - 2), k + 1):
                    print(("  > " if kk == k else "    ") + src.fmt(kk))
                print(f"\n  --- {seqs_b[1]} (stayed) ---")
                for kk in range(max(0, oth_i - 2), min(len(oth.states), oth_i + 2)):
                    print("    " + oth.fmt(kk))
                return 1
            # Not a control-flow split (same PCs both sides), but did the skipped
            # records WRITE a span of memory? A fill/copy loop run a different
            # number of times = different memory written = a REAL divergence,
            # even though both sides re-sync to identical registers at loop exit.
            adv = addr_advances(src_states, rng) if not args.no_split_detect else None
            if adv:
                n_reg, v0, v1, stride = adv
                src = O if side == "oracle" else N
                oth = N if side == "oracle" else O
                k0 = rng.start
                oth_i = i if side == "oracle" else j
                print(f"\n*** WRITING-LOOP DIVERGENCE — {side} ran a fill/copy loop "
                      f"{d} records the other side did not ***")
                print(f"  at {seqs_a[1]} seq {seqs_a[0]} A{n_reg} marches "
                      f"${v0:08X} -> ${v1:08X} (stride {stride:+d}, "
                      f"{abs(v1 - v0)} bytes) while {seqs_b[1]} runs fewer iterations.")
                print(f"  register-alignment masked this: a poll loop spins in place (benign), "
                      f"but a marching pointer means a SPAN of memory was written a different "
                      f"number of times — the real data split behind a later "
                      f"same-register/different-memory divergence.")
                print(f"  {resyncs} benign re-syncs skipped earlier; max spin skew {max_skew}.")
                print(f"\n  --- {side} (ran the extra iterations) ---")
                for kk in range(max(0, k0 - 1), min(len(src.states), rng.stop + 1)):
                    print(("  > " if kk in rng else "    ") + src.fmt(kk))
                print(f"\n  --- {seqs_b[1]} (ran fewer) ---")
                for kk in range(max(0, oth_i - 2), min(len(oth.states), oth_i + 2)):
                    print("    " + oth.fmt(kk))
                return 1
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
