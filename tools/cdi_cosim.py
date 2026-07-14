#!/usr/bin/env python3
"""cdi_cosim.py — differential co-simulation coordinator (docs/COSIM-SPEC.md).

Upgrades the register-only realignment in `realign_divergence.py` to comparing
FULL architectural state per retired instruction: CPU registers (d0..d7, a0..a7,
sr, pc, usp, ssp — including the INACTIVE stack-pointer shadow, the current
SSP-frontier blind spot) PLUS two guest-RAM incremental hashes (ram0_h, ram1_h).
A "same registers, different memory" divergence — invisible to the old tool —
now shows up as a named ram0_h/ram1_h split. See COSIM-SPEC.md §1/§4/§6/§7 for
the wire contract and behavior this file implements; that document is the
single source of truth for field names — this file must not drift from it.

Reused from realign_divergence.py (per COSIM-SPEC.md, "extend, don't reinvent"):
  - the persistent-connection TCP client that pages `trace {from,count:256}`
    (records cap at 256/call; page with from = last seq + 1) -> TraceClient
  - the JSON line-protocol conventions shared by every tools/*.py client
  - the general shape of a bounded re-alignment search over two seq streams
realign_divergence.py has no embedded self-test to carry over (checked; none
exists) — the --self-test suite here is new, built to COSIM-SPEC.md's four
required synthetic cases (see _self_test() at the bottom).

Doctrine (CLAUDE.md / COSIM-SPEC.md §0): both sides FREE-RUN with always-on
rings; this coordinator only QUERIES rings (paged `trace`) and diffs. The one
"stop" primitive is each side's existing `--stop-seq --hold` freeze, used here
only to give the gates a deterministic window to pull from a ring that has been
recording since seq 0 — never to lock-step two live observers.

Subcommands (COSIM-SPEC.md §6/§7):
    diff   <native_port> <oracle_port>                       the core diff
    gate1  <oracle_exe>  <rom> [seq]                          oracle self-determinism
    gate2  <native_exe>  <rom> [seq]                          native self-determinism
    gate3  <exe> <rom> <seq> <kind:ram|reg> <idx> <xorhex>    fault-injection localizes
    gate4  <exe> <rom> [seq]                                  hash-vs-byte audit
    gates  <native_exe> <oracle_exe> <rom> [seq]               run 1..4 in order
    --self-test                                                synthetic self-tests, no sockets

Usage:
    python tools/cdi_cosim.py --self-test
    python tools/cdi_cosim.py diff 4380 4381
    python tools/cdi_cosim.py gates build/runner/CdiRuntime.exe build/oracle/CdiOracle.exe bios/cdi490a.rom
"""
import argparse, collections, json, secrets, socket, subprocess, sys, threading, time
from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# §2a/§4: state surface field names/order — the SINGLE definition every other
# function in this file works from. Keep in lock-step with COSIM-SPEC.md.
# --------------------------------------------------------------------------
CPU_KEYS = [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)] + ["sr", "pc", "usp", "ssp"]
RAM_KEYS = ["ram0_h", "ram1_h"]
FULL_KEYS = CPU_KEYS + RAM_KEYS          # the exact-match surface (§6)
TCYC_KEY = "tcyc"                        # informational only — §1, never in FULL_KEYS


def _hash_int(v):
    """Normalize a ram*_h value (16-char lowercase hex STRING on the wire,
    §4) to an int for comparison. Also accepts a bare int (synthetic tests /
    already-parsed callers) so the core doesn't care which form it was given."""
    if v is None:
        return None
    if isinstance(v, str):
        return int(v, 16)
    return int(v)


def full_state(rec):
    """The §6 exact-match tuple: CPU fields as-is + both RAM hashes normalized
    to int. Two records are IDENTICAL guest state iff this tuple matches."""
    vals = [rec.get(k) for k in CPU_KEYS]
    vals += [_hash_int(rec.get(k)) for k in RAM_KEYS]
    return tuple(vals)


def _first_diff_signal(nrec, orec):
    """Which named signal split first, scanning CPU fields in §2a table order
    then the two RAM banks (§2b). Returns a field name like 'ssp', 'ram0_h', or
    None if the records are actually equal (caller error)."""
    for k in CPU_KEYS:
        if nrec.get(k) != orec.get(k):
            return k
    for k in RAM_KEYS:
        if _hash_int(nrec.get(k)) != _hash_int(orec.get(k)):
            return k
    return None


def _expected_signal_for_injection(kind, idx):
    """§5 --cosim-inject idx -> the field name the diff should localize to.
    Pure mapping, used by gate3 to predict the signal before running anything."""
    if kind == "reg":
        if 0 <= idx <= 7:
            return f"d{idx}"
        if 8 <= idx <= 15:
            return f"a{idx - 8}"
        if idx == 16:
            return "usp"
        if idx == 17:
            return "ssp"
        raise ValueError(f"reg idx {idx} out of range 0..17 (COSIM-SPEC.md §5)")
    if kind == "ram":
        if 0x00000000 <= idx <= 0x0007FFFF:
            return "ram0_h"
        if 0x00200000 <= idx <= 0x0027FFFF:
            return "ram1_h"
        raise ValueError(f"ram addr {idx:#x} is outside both hashed banks (COSIM-SPEC.md §2b)")
    raise ValueError(f"bad injection kind {kind!r} (must be 'ram' or 'reg')")


# --------------------------------------------------------------------------
# Diff core — pure, socket-free, operates on lists of record dicts. This is
# the part the --self-test suite exercises directly (no TCP, no subprocess).
# --------------------------------------------------------------------------
@dataclass
class DiffOutcome:
    ok: bool
    divergence_seq_native: int = None
    divergence_seq_oracle: int = None
    signal: str = None
    resyncs: list = field(default_factory=list)      # [(side, delta, native_seq, oracle_seq), ...]
    tcyc_drifts: list = field(default_factory=list)  # [(native_seq, oracle_seq, ntcyc, otcyc), ...]
    context: dict = field(default_factory=dict)       # {"native": [...], "oracle": [...]}, divergent rec last
    compared: int = 0
    message: str = ""


def _confirm(native, oracle, i0, j0, confirm_n):
    """A candidate resync at (i0,j0) is accepted only if it holds for up to
    `confirm_n` FOLLOWING record pairs too (or runs out of loaded records
    trying) — guards against a single coincidental full-state match (20 CPU
    fields + 2x64-bit hash) being mistaken for a real re-sync. Given the state
    surface size a false positive here is already astronomically unlikely;
    this is defense in depth, not the primary signal."""
    for c in range(confirm_n):
        ii, jj = i0 + c, j0 + c
        if ii >= len(native) or jj >= len(oracle):
            break   # loaded window ended before we could confirm further; accept
        if full_state(native[ii]) != full_state(oracle[jj]):
            return False
    return True


def _find_resync(native, oracle, i, j, drift, confirm_n):
    """§6 bounded adaptive offset: try shifting ONE side ahead by delta in
    1..drift (both directions) and see if that restores a full-state match
    (confirmed for a few records after). The current (i,j) pointers already
    encode every previously-bridged offset, so this always searches ±drift
    around the CURRENT running offset, not a fixed global one."""
    for delta in range(1, drift + 1):
        # oracle ran `delta` extra records before native's next one shows up
        if j + delta < len(oracle) and full_state(native[i]) == full_state(oracle[j + delta]):
            if _confirm(native, oracle, i, j + delta, confirm_n):
                return ("oracle", delta)
        # native ran `delta` extra records before oracle's next one shows up
        if i + delta < len(native) and full_state(native[i + delta]) == full_state(oracle[j]):
            if _confirm(native, oracle, i + delta, j, confirm_n):
                return ("native", delta)
    return None


def _build_context(native, oracle, i, j, n):
    lo_n, lo_o = max(0, i - n), max(0, j - n)
    return {"native": native[lo_n:i + 1], "oracle": oracle[lo_o:j + 1]}


def diff_core(native, oracle, drift=3, confirm=2, context_n=6):
    """The §6 alignment/diff algorithm. `native`/`oracle` are seq-ascending
    lists of record dicts (as parsed from `trace` JSON, or synthetic). Returns
    a DiffOutcome; never raises on a normal divergence (that's the expected
    result, not an error)."""
    i = j = 0
    resyncs = []
    tcyc_drifts = []
    compared = 0
    while i < len(native) and j < len(oracle):
        if full_state(native[i]) == full_state(oracle[j]):
            nt, ot = native[i].get(TCYC_KEY), oracle[j].get(TCYC_KEY)
            if nt is not None and ot is not None and nt != ot:
                tcyc_drifts.append((native[i]["seq"], oracle[j]["seq"], nt, ot))
            i += 1
            j += 1
            compared += 1
            continue
        hit = _find_resync(native, oracle, i, j, drift, confirm)
        if hit is None:
            sig = _first_diff_signal(native[i], oracle[j])
            ctx = _build_context(native, oracle, i, j, context_n)
            return DiffOutcome(
                False, native[i]["seq"], oracle[j]["seq"], sig, resyncs, tcyc_drifts, ctx, compared,
                message=f"divergence at native seq {native[i]['seq']} / oracle seq {oracle[j]['seq']}: "
                        f"signal {sig}")
        side, delta = hit
        resyncs.append((side, delta, native[i]["seq"], oracle[j]["seq"]))
        if side == "oracle":
            j += delta
        else:
            i += delta
    return DiffOutcome(
        True, resyncs=resyncs, tcyc_drifts=tcyc_drifts, compared=compared,
        message=f"no divergence in loaded window ({compared} aligned matches, {len(resyncs)} benign re-syncs)")


# --------------------------------------------------------------------------
# Formatting (used by the `diff` CLI; kept separate from diff_core so the core
# stays a plain data-in/data-out function for the self-tests).
# --------------------------------------------------------------------------
def fmt_rec(r):
    d = " ".join(f"D{k}={r.get('d' + str(k), 0):08X}" for k in range(8))
    a = " ".join(f"A{k}={r.get('a' + str(k), 0):08X}" for k in range(8))
    return (f"seq {r.get('seq'):>8} PC=${r.get('pc', 0):08X} SR=${r.get('sr', 0):04X} "
            f"USP=${r.get('usp', 0):08X} SSP=${r.get('ssp', 0):08X} tcyc={r.get('tcyc', '?')}\n"
            f"          {d}\n          {a}\n"
            f"          ram0_h={r.get('ram0_h', '?')} ram1_h={r.get('ram1_h', '?')}")


def print_report(out: DiffOutcome, native_port, oracle_port):
    if out.ok:
        print(f"\nno divergence across the loaded window: {out.message}.")
        if out.tcyc_drifts:
            mx = max(abs(nt - ot) for _, _, nt, ot in out.tcyc_drifts)
            print(f"  tcyc informational drift: {len(out.tcyc_drifts)} aligned seq disagree "
                  f"on tcyc (max |drift|={mx}). Device-timing finding only, not a state fail (§1).")
        return
    print(f"\n*** DIVERGENCE — native seq {out.divergence_seq_native} vs oracle seq "
          f"{out.divergence_seq_oracle} (signal: {out.signal}) ***")
    print(f"  {out.compared} aligned matches and {len(out.resyncs)} benign re-syncs bridged before this "
          f"point; no offset within the search bound restores a full-state (CPU+RAM) match.")
    if out.signal in RAM_KEYS:
        print(f"  RAM split: re-run each side to --stop-seq {out.divergence_seq_native}, read_mem the "
              f"suspect bank region, diff bytes (COSIM-SPEC.md §6 drill-down).")
    else:
        print(f"  CPU split: register {out.signal} is the answer — the instruction that ran just before "
              f"this seq wrote it wrong.")
    for label, port in (("native", native_port), ("oracle", oracle_port)):
        recs = out.context.get(label, [])
        print(f"\n  --- {label} (:{port}) ---")
        for k, r in enumerate(recs):
            marker = "  > " if k == len(recs) - 1 else "    "
            print(marker + fmt_rec(r))
    if out.tcyc_drifts:
        print(f"\n  (tcyc also drifted at {len(out.tcyc_drifts)} aligned seq before this point — "
              f"informational, ignored for this verdict.)")


# --------------------------------------------------------------------------
# Thin socket/launch layer. Nothing above this line touches a socket or a
# subprocess — diff_core is testable without either.
# --------------------------------------------------------------------------
def _rpc(port, obj, host="127.0.0.1", timeout=10, max_response=1 << 20):
    deadline = time.monotonic() + timeout
    with socket.create_connection((host, port), timeout=timeout) as s:
        remain = deadline - time.monotonic()
        if remain <= 0:
            raise TimeoutError(f":{port} connect exceeded {timeout}s")
        s.settimeout(remain)
        s.sendall((json.dumps(obj) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise TimeoutError(f":{port} response exceeded {timeout}s")
            s.settimeout(remain)
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            if len(buf) > max_response:
                raise ValueError(f":{port} response exceeded {max_response} bytes")
    line = buf.split(b"\n", 1)[0]
    return json.loads(line.decode()) if line.strip() else {}


def wait_for_port(port, timeout=30.0, host="127.0.0.1", expected_session=None):
    """Poll for the CD-i listener to bind and answer its identity ping.
    A bare TCP connect can hit an unrelated process already holding the port.
    This is startup synchronization
    for a subprocess WE just launched, not the arm-then-capture observability
    anti-pattern (CLAUDE.md): the ring is always-on from seq 0 inside the
    process regardless of when the TCP listener opens, so no events are lost
    waiting for the socket — we just can't ask for them until it's up."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            remain = max(0.01, min(1.0, deadline - time.monotonic()))
            reply = _rpc(port, {"cmd": "ping"}, host=host, timeout=remain)
            if (isinstance(reply, dict) and reply.get("ok") and reply.get("pong")
                    and reply.get("session") == expected_session):
                return True
        except (OSError, ValueError, TypeError, AttributeError):
            pass
        time.sleep(0.1)
    return False


class TraceClient:
    """Persistent-connection bulk reader over one side's debug server. Pages
    `trace {from,count:256}` (server caps at 256/call regardless of the count
    requested) and returns raw record dicts — reuses the connection-per-session
    pattern from realign_divergence.py's Trace class, generalized to the full
    COSIM-SPEC record shape instead of a fixed register tuple."""

    def __init__(self, port, host="127.0.0.1", connect_timeout=10):
        self.port = port
        self.sock = socket.create_connection((host, port), timeout=connect_timeout)
        self.rbuf = b""

    def _call(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())
        while b"\n" not in self.rbuf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise IOError(f":{self.port} closed the connection mid-response")
            self.rbuf += chunk
        line, self.rbuf = self.rbuf.split(b"\n", 1)
        return json.loads(line.decode())

    def total(self):
        return self._call({"cmd": "status", "blocks": 0}).get("blocks", 0)

    def full_ram_hash(self):
        """§5 cosim_full_ram_hash — full recompute over LIVE RAM, for gate4."""
        return self._call({"cmd": "cosim_full_ram_hash"})

    def load(self, start, stop):
        """Page [start, stop) forward. Raises IOError if the window was already
        evicted from the ring (the caller asked for history the ring no longer
        holds — start the run with a bigger ring or query sooner)."""
        out = []
        cur = start
        while cur < stop:
            resp = self._call({"cmd": "trace", "from": cur, "count": 256})
            recs = resp.get("records", [])
            if not recs:
                break
            if recs[0]["seq"] > cur:
                raise IOError(f"seq {cur} evicted from :{self.port} (oldest live is {recs[0]['seq']})")
            done = False
            for r in recs:
                if r["seq"] < cur:
                    continue
                if r["seq"] >= stop:
                    done = True
                    break
                out.append(r)
            if done:
                break
            cur = recs[-1]["seq"] + 1
        return out

    def get(self, seq):
        recs = self.load(seq, seq + 1)
        return recs[0] if recs else None

    def cycle_page(self, start, stop):
        """Read one compact `cycle_trace` page beginning exactly at start."""
        resp = self._call({"cmd": "cycle_trace", "from": start, "count": 512})
        if not resp.get("ok"):
            raise IOError(f":{self.port} cycle_trace failed: {resp}")
        recs = [r for r in resp.get("records", []) if r["seq"] < stop]
        if recs and recs[0]["seq"] != start:
            raise IOError(
                f"seq {start} unavailable from :{self.port} cycle_trace "
                f"(first returned is {recs[0]['seq']})")
        return recs

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def _validate_fields(rec, port):
    missing = [k for k in FULL_KEYS if k not in rec]
    if missing:
        raise IOError(
            f":{port} trace records are missing {missing} — this coordinator requires the "
            f"COSIM-SPEC.md §4 fields (usp, ssp, ram0_h, ram1_h). Rebuild the runner/oracle "
            f"against docs/COSIM-SPEC.md before running cdi_cosim.py.")


# --------------------------------------------------------------------------
# `diff` subcommand — live two-port diff.
# --------------------------------------------------------------------------
@dataclass
class CycleGroup:
    executions: int = 0
    mismatches: int = 0
    contribution: int = 0
    abs_contribution: int = 0
    first_seq: int = 0
    pairs: dict = field(default_factory=dict)


@dataclass
class CycleAudit:
    transitions: int = 0
    aligned: int = 0
    skipped: int = 0
    mismatches: int = 0
    contribution: int = 0
    first: tuple = None
    groups: dict = field(default_factory=dict)
    resyncs: int = 0
    native_skips: int = 0
    oracle_skips: int = 0
    filtered_seams: int = 0


def _cycle_audit_priced(audit, nrec, orec):
    audit.transitions += 1
    audit.aligned += 1
    if not nrec.get("cost_valid", True) or not orec.get("cost_valid", True):
        audit.filtered_seams += 1
        return
    ncost, ocost = int(nrec["cost"]), int(orec["cost"])
    key = (int(nrec["pc"]), int(nrec.get("op", 0)))
    group = audit.groups.setdefault(key, CycleGroup())
    group.executions += 1
    if ncost == ocost:
        return
    delta = ncost - ocost
    audit.mismatches += 1
    audit.contribution += delta
    group.mismatches += 1
    group.contribution += delta
    group.abs_contribution += abs(delta)
    if group.first_seq == 0:
        group.first_seq = int(nrec["seq"])
    pair = (ncost, ocost)
    group.pairs[pair] = group.pairs.get(pair, 0) + 1
    if audit.first is None:
        audit.first = (int(nrec["seq"]), int(orec["seq"]), key[0], key[1], ncost, ocost)


def _cycle_audit_add(audit, prev_n, prev_o, cur_n, cur_o):
    """Consume one N->N+1 transition. Costs belong to the previous record."""
    audit.transitions += 1
    consecutive = (cur_n["seq"] == prev_n["seq"] + 1 and
                   cur_o["seq"] == prev_o["seq"] + 1)
    same_index = prev_n["seq"] == prev_o["seq"] and cur_n["seq"] == cur_o["seq"]
    same_instruction = (prev_n["pc"] == prev_o["pc"] and
                        prev_n.get("op") == prev_o.get("op"))
    if not (consecutive and same_index and same_instruction):
        audit.skipped += 1
        return

    pn = dict(prev_n)
    po = dict(prev_o)
    pn["cost"] = int(cur_n["tcyc"]) - int(prev_n["tcyc"])
    po["cost"] = int(cur_o["tcyc"]) - int(prev_o["tcyc"])
    _cycle_audit_priced(audit, pn, po)
    audit.transitions -= 1  # already counted above


def _print_cycle_audit(audit, start, stop, top):
    print(f"cycle audit [{start}, {stop}): transitions={audit.transitions} "
          f"aligned={audit.aligned} skipped={audit.skipped} resyncs={audit.resyncs} "
          f"(native-extra={audit.native_skips}, oracle-extra={audit.oracle_skips}) "
          f"exception-seam-filtered={audit.filtered_seams}")
    print(f"mismatched costs={audit.mismatches}; cumulative native-oracle "
          f"contribution={audit.contribution:+d} cycles")
    if audit.first:
        nseq, oseq, pc, op, nc, oc = audit.first
        print(f"first mismatch: native-seq={nseq} oracle-seq={oseq} "
              f"PC=${pc:06X} op=${op:04X} "
              f"native={nc} oracle={oc} delta={nc-oc:+d}")
    if not audit.mismatches:
        return

    rows = [(key, g) for key, g in audit.groups.items() if g.mismatches]
    rows.sort(key=lambda item: (-abs(item[1].contribution),
                                -item[1].abs_contribution,
                                -item[1].mismatches, item[0]))
    print(f"\ntop {min(top, len(rows))} PCs/opcodes by |net cycle contribution|:")
    print("  PC      op    mism/executions  net      abs      first     cost pairs native/oracle x count")
    for (pc, op), g in rows[:top]:
        pairs = sorted(g.pairs.items(), key=lambda item: (-item[1], item[0]))
        pair_text = ", ".join(f"{nc}/{oc}x{count}" for (nc, oc), count in pairs[:6])
        if len(pairs) > 6:
            pair_text += f", +{len(pairs)-6} more"
        print(f"  ${pc:06X} ${op:04X} {g.mismatches:7d}/{g.executions:<9d} "
              f"{g.contribution:+8d} {g.abs_contribution:8d} {g.first_seq:9d}  {pair_text}")


def _priced_cycle_records(client, start, stop):
    """Yield compact records with the entry PC/opcode's cost attached."""
    cur, prev = start, None
    while cur < stop:
        page = client.cycle_page(cur, stop)
        if not page:
            break
        for rec in page:
            if prev is not None:
                priced = dict(prev)
                priced["cost"] = int(rec["tcyc"]) - int(prev["tcyc"])
                priced["cost_valid"] = not (prev.get("cycle_seam", 0) or
                                             rec.get("cycle_seam", 0))
                yield priced
            prev = rec
        cur = page[-1]["seq"] + 1


class _CycleBuffer:
    def __init__(self, records):
        self.records = iter(records)
        self.buf = []
        self.pos = 0

    def peek(self, offset=0):
        want = self.pos + offset
        while len(self.buf) <= want:
            try:
                self.buf.append(next(self.records))
            except StopIteration:
                return None
        return self.buf[want]

    def pop(self, count=1):
        self.pos += count
        if self.pos >= 8192:
            del self.buf[:self.pos]
            self.pos = 0


def _cycle_key(rec):
    return (rec["pc"], rec.get("op")) if rec is not None else None


def _cycle_confirm(nbuf, noff, obuf, ooff, count):
    for k in range(count):
        nr, orr = nbuf.peek(noff + k), obuf.peek(ooff + k)
        if nr is None or orr is None or _cycle_key(nr) != _cycle_key(orr):
            return False
    return True


def cmd_cycles(args):
    try:
        N, O = TraceClient(args.native_port), TraceClient(args.oracle_port)
    except OSError as e:
        print(f"cannot reach a server ({e}). Start both with --stop-seq/--hold first.", file=sys.stderr)
        return 2
    audit = CycleAudit()
    try:
        nt, ot = N.total(), O.total()
        stop = min(nt, ot)
        if args.stop is not None:
            stop = min(stop, args.stop)
        if stop <= args.start + 1:
            raise IOError(f"cycle window [{args.start}, {stop}) has fewer than two records")
        nb = _CycleBuffer(_priced_cycle_records(N, args.start, stop))
        ob = _CycleBuffer(_priced_cycle_records(O, args.start, stop))
        next_progress = args.start + args.progress if args.progress else 0
        while nb.peek() is not None and ob.peek() is not None:
            nr, orr = nb.peek(), ob.peek()
            if _cycle_key(nr) == _cycle_key(orr):
                _cycle_audit_priced(audit, nr, orr)
                nb.pop(); ob.pop()
                if next_progress and max(nr["seq"], orr["seq"]) >= next_progress:
                    print(f"  aligned through native seq {nr['seq']}, oracle seq {orr['seq']} ...", flush=True)
                    next_progress += args.progress
                continue

            hit = None
            for d in range(1, args.window + 1):
                if _cycle_confirm(nb, 0, ob, d, args.confirm):
                    hit = ("oracle", d); break
                if _cycle_confirm(nb, d, ob, 0, args.confirm):
                    hit = ("native", d); break
                if d <= 16 and _cycle_confirm(nb, d, ob, d, args.confirm):
                    hit = ("both", d); break
            if hit is None:
                raise IOError(
                    f"cannot re-align within {args.window} records at native seq {nr['seq']} "
                    f"PC=${nr['pc']:06X} op=${nr.get('op', 0):04X} vs oracle seq {orr['seq']} "
                    f"PC=${orr['pc']:06X} op=${orr.get('op', 0):04X}")
            side, count = hit
            audit.resyncs += 1
            if side in ("native", "both"):
                audit.native_skips += count
                audit.skipped += count
                nb.pop(count)
            if side in ("oracle", "both"):
                audit.oracle_skips += count
                audit.skipped += count
                ob.pop(count)
    except (OSError, IOError) as e:
        print(f"cycle audit failed: {e}", file=sys.stderr)
        return 2
    finally:
        N.close()
        O.close()
    # stop is the exclusive RECORD bound; the final record only supplies the
    # delta endpoint, so the priced instruction interval ends at stop-1.
    _print_cycle_audit(audit, args.start, stop - 1, args.top)
    return 0


def _wait_for_blocks(port, target, timeout):
    deadline = time.monotonic() + timeout
    last = -1
    while time.monotonic() < deadline:
        try:
            remain = max(0.01, min(2.0, deadline - time.monotonic()))
            last = int(_rpc(port, {"cmd": "status"}, timeout=remain).get("blocks", -1))
            if last >= target:
                return last
        except (OSError, ValueError):
            pass
        time.sleep(0.1)
    raise RuntimeError(f":{port} did not park at {target} records within {timeout}s (last={last})")


def cmd_cycle_run(args):
    """Free-run both sides, park their always-on rings, audit, then clean up."""
    nproc = oproc = None
    try:
        nproc = _run_hold(args.native_exe, args.rom, args.native_port,
                          args.seq, launch_timeout=args.timeout)
        oproc = _run_hold(args.oracle_exe, args.rom, args.oracle_port,
                          args.seq, launch_timeout=args.timeout)
        nb = _wait_for_blocks(args.native_port, args.seq, args.timeout)
        ob = _wait_for_blocks(args.oracle_port, args.seq, args.timeout)
        print(f"parked native={nb} oracle={ob}; auditing ...", flush=True)
        audit_args = argparse.Namespace(
            native_port=args.native_port, oracle_port=args.oracle_port,
            start=args.start, stop=args.seq, top=args.top, progress=args.progress,
            window=args.window, confirm=args.confirm)
        return cmd_cycles(audit_args)
    finally:
        if oproc is not None:
            _stop(oproc, args.oracle_port)
        if nproc is not None:
            _stop(nproc, args.native_port)


def cmd_diff(args):
    try:
        N, O = TraceClient(args.native_port), TraceClient(args.oracle_port)
    except OSError as e:
        print(f"cannot reach a server ({e}). Start both with --stop-seq/--hold first.", file=sys.stderr)
        return 2
    try:
        nt, ot = N.total(), O.total()
        stop = min(nt, ot)
        if args.limit:
            stop = min(stop, args.start + args.limit)
        print(f"native :{args.native_port} has {nt}; oracle :{args.oracle_port} has {ot}. "
              f"loading [{args.start}, {stop}) ...", flush=True)
        native = N.load(args.start, stop)
        oracle = O.load(args.start, stop)
    except (OSError, IOError) as e:
        print(f"load failed: {e}", file=sys.stderr)
        return 2
    finally:
        N.close()
        O.close()

    if not native or not oracle:
        print("no records loaded in the requested window.", file=sys.stderr)
        return 2
    try:
        _validate_fields(native[0], args.native_port)
        _validate_fields(oracle[0], args.oracle_port)
    except IOError as e:
        print(str(e), file=sys.stderr)
        return 2

    print(f"loaded native={len(native)} oracle={len(oracle)} records; aligning "
          f"(drift<=±{args.drift}, confirm={args.confirm}) ...", flush=True)
    out = diff_core(native, oracle, drift=args.drift, confirm=args.confirm, context_n=args.context)
    print_report(out, args.native_port, args.oracle_port)
    return 0 if out.ok else 1


# --------------------------------------------------------------------------
# Gate launch/collect helpers (§7).
# --------------------------------------------------------------------------
def _reap_launched(proc, output_thread=None):
    """Best-effort hard cleanup for every failed/interrupted launch path."""
    if proc is None:
        return
    try:
        if proc.poll() is None:
            proc.kill()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
    finally:
        if output_thread is not None and output_thread.is_alive():
            output_thread.join(timeout=1)
        if proc.stdout is not None and (output_thread is None or not output_thread.is_alive()):
            try:
                proc.stdout.close()
            except OSError:
                pass


def _run_hold(exe, rom, port, stop_seq, extra_args=None, launch_timeout=60.0):
    session = secrets.token_hex(16)
    args = [exe, rom, "--port", str(port), "--stop-seq", str(stop_seq), "--hold",
            "--cosim-session", session]
    if extra_args:
        args += extra_args
    proc = output_thread = None
    try:
        # The co-sim's evidence channel is the always-on TCP ring, not child
        # stdio.  Drain a merged pipe continuously into a bounded in-memory
        # tail: long oracle captures cannot block on backpressure, routine
        # console chatter stays quiet, and launch failures retain diagnostics
        # without creating a log file.
        proc = subprocess.Popen(args, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, bufsize=0)
        output_tail = collections.deque(maxlen=16)  # 16 fixed 4-KiB chunks
        output_lock = threading.Lock()

        def drain_output():
            try:
                while True:
                    chunk = proc.stdout.read(4096)
                    if not chunk:
                        break
                    with output_lock:
                        output_tail.append(chunk)
            finally:
                proc.stdout.close()

        output_thread = threading.Thread(target=drain_output,
                                         name=f"cosim-output-{port}",
                                         daemon=True)
        output_thread.start()
        proc._cosim_output_tail = output_tail
        proc._cosim_output_lock = output_lock
        proc._cosim_output_thread = output_thread
    except OSError as e:
        _reap_launched(proc, output_thread)
        raise RuntimeError(f"failed to launch {exe}: {e}") from e
    except BaseException:
        _reap_launched(proc, output_thread)
        raise

    try:
        ready = wait_for_port(port, launch_timeout, expected_session=session)
    except BaseException as e:
        if not isinstance(e, Exception):
            _reap_launched(proc, output_thread)
            raise
        ready = False
        readiness_error = e
    else:
        readiness_error = None
    if not ready:
        launch_rc = proc.poll()
        _reap_launched(proc, output_thread)
        state = f"exit={launch_rc}" if launch_rc is not None else "process still running"
        with output_lock:
            diagnostic = b"".join(output_tail).decode(errors="replace").rstrip()
        if diagnostic:
            diagnostic = f"\nlast child output:\n{diagnostic}"
        if readiness_error is not None:
            diagnostic += f"\nreadiness error: {readiness_error}"
        raise RuntimeError(
            f"{exe} did not establish session {session} on :{port} within {launch_timeout}s "
            f"({state}){diagnostic}")
    proc._cosim_session = session
    return proc


def _stop(proc, port):
    try:
        try:
            _rpc(port, {"cmd": "quit"}, timeout=2)
        except (OSError, ValueError, TypeError):
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
    finally:
        output_thread = getattr(proc, "_cosim_output_thread", None)
        if output_thread is not None:
            output_thread.join(timeout=1)


def _collect(exe, rom, port, stop_seq, extra_args=None, start=1, timeout=180.0):
    """Launch exe --stop-seq stop_seq --hold, pull [start, stop_seq], quit it."""
    proc = _run_hold(exe, rom, port, stop_seq, extra_args, launch_timeout=timeout)
    try:
        _wait_for_blocks(port, stop_seq, timeout)
        tc = TraceClient(port)
        try:
            recs = tc.load(start, stop_seq + 1)
        finally:
            tc.close()
    finally:
        _stop(proc, port)
    if recs:
        _validate_fields(recs[0], port)
    return recs


def _assert_identical_streams(a, b):
    """§7 gate1/gate2: two runs of the SAME side must produce byte-identical
    CPU+ram0_h+ram1_h streams. tcyc is intentionally excluded (informational,
    §1) — a and b are the SAME binary/ROM so it should match too, but that is
    not what this gate is chartered to enforce."""
    if len(a) != len(b):
        return False, f"record count differs: run A={len(a)} run B={len(b)}"
    for ra, rb in zip(a, b):
        if ra["seq"] != rb["seq"]:
            return False, f"seq sequence itself diverges: {ra['seq']} vs {rb['seq']}"
        if full_state(ra) != full_state(rb):
            sig = _first_diff_signal(ra, rb)
            return False, (f"non-deterministic at seq {ra['seq']}: {sig} differs "
                            f"(run A={ra.get(sig)!r}, run B={rb.get(sig)!r})")
    return True, f"{len(a)} records byte-identical (CPU + ram0_h + ram1_h)"


# --------------------------------------------------------------------------
# gate1 / gate2 — self-determinism
# --------------------------------------------------------------------------
def cmd_gate1(args):
    print(f"[gate1] oracle self-determinism: {args.oracle_exe} vs itself, rom={args.rom}, "
          f"seq window [1,{args.seq}] (x2 runs)")
    a = _collect(args.oracle_exe, args.rom, args.base_port, args.seq)
    b = _collect(args.oracle_exe, args.rom, args.base_port + 1, args.seq)
    ok, msg = _assert_identical_streams(a, b)
    print(f"[gate1] {'PASS' if ok else 'FAIL'}: {msg}")
    return 0 if ok else 1


def cmd_gate2(args):
    print(f"[gate2] native self-determinism: {args.native_exe} vs itself, rom={args.rom}, "
          f"seq window [1,{args.seq}] (x2 runs)")
    a = _collect(args.native_exe, args.rom, args.base_port, args.seq)
    b = _collect(args.native_exe, args.rom, args.base_port + 1, args.seq)
    ok, msg = _assert_identical_streams(a, b)
    print(f"[gate2] {'PASS' if ok else 'FAIL'}: {msg}")
    return 0 if ok else 1


# --------------------------------------------------------------------------
# gate3 — fault injection localizes
# --------------------------------------------------------------------------
def cmd_gate3(args):
    try:
        expected = _expected_signal_for_injection(args.kind, args.idx)
    except ValueError as e:
        print(f"[gate3] FAIL: {e}", file=sys.stderr)
        return 1

    stop = args.seq + args.margin
    print(f"[gate3] fault injection: {args.exe} kind={args.kind} idx={args.idx:#x} "
          f"xor={args.xorhex:#x} at seq {args.seq} (expect signal={expected}); window up to {stop}")

    baseline = _collect(args.exe, args.rom, args.base_port, stop)
    inject_str = f"{args.seq}:{args.kind}:0x{args.idx:x}:0x{args.xorhex:x}"
    injected = _collect(args.exe, args.rom, args.base_port + 1, stop,
                         extra_args=["--cosim-inject", inject_str])

    out = diff_core(baseline, injected, drift=args.drift, confirm=1)
    if out.ok:
        print(f"[gate3] FAIL: injected run never diverged from baseline within seq window "
              f"[1,{stop}) — the injection had no observable effect (possible no-op / blind hasher).")
        return 1

    seq_ok = abs(out.divergence_seq_native - args.seq) <= args.tolerance
    if not seq_ok:
        print(f"[gate3] FAIL: divergence localized to seq {out.divergence_seq_native}, expected "
              f"seq {args.seq} (tolerance {args.tolerance}).")
        return 1
    if out.signal != expected:
        print(f"[gate3] FAIL: divergence signal is {out.signal!r}, expected {expected!r}.")
        return 1
    print(f"[gate3]   halts at seq {out.divergence_seq_native}, signal={out.signal} — matches prediction.")

    if args.kind == "ram":
        later = args.seq + max(1, min(10, args.margin - 1))
        b_rec = next((r for r in baseline if r["seq"] == later), None)
        i_rec = next((r for r in injected if r["seq"] == later), None)
        if b_rec is None or i_rec is None:
            print(f"[gate3] FAIL: no record at seq {later} to check persistence (margin too small?).")
            return 1
        if _hash_int(b_rec.get(expected)) == _hash_int(i_rec.get(expected)):
            print(f"[gate3] FAIL: {expected} re-converged by seq {later} — the flip did not "
                  f"persist/cascade in the incremental hash (possible blind/constant hasher).")
            return 1
        print(f"[gate3]   persistence check: {expected} still differs at seq {later} "
              f"(incremental page-hash actually recomputed the touched page).")

    print(f"[gate3] PASS: fault injection localizes correctly.")
    return 0


# --------------------------------------------------------------------------
# gate4 — hash-vs-byte audit
# --------------------------------------------------------------------------
def _gate4_verify(full, min_seq=None):
    """§5 gate4 predicate, pure/socket-free: given a parsed `cosim_full_ram_hash`
    response, assert ram0_h == ram0_h_inc and ram1_h == ram1_h_inc FROM THE SAME
    RESPONSE (one TCP round-trip, one instant). This is the off-by-one-immune
    comparison — it replaces the old approach of comparing the full recompute
    against a separately-pulled `trace` ring record, which had a one-instruction
    skew (parked state is pre-seq-N, the last ring record is pre-seq-(N-1)).
    Extracted from cmd_gate4 so --self-test can exercise the actual comparison
    logic without a live process. Returns (ok, reason)."""
    if not full or not full.get("ok"):
        return False, f"no/invalid response: {full!r}"
    if min_seq is not None:
        try:
            response_seq = int(full.get("seq", -1))
        except (TypeError, ValueError):
            response_seq = -1
        if response_seq < min_seq:
            return False, (f"response is from seq {response_seq}, before requested checkpoint "
                           f"{min_seq} — the process was queried while still running")
    missing = [f for f in ("ram0_h", "ram1_h", "ram0_h_inc", "ram1_h_inc") if f not in full]
    if missing:
        return False, (f"response missing {missing} — rebuild against COSIM-SPEC.md §5 "
                        f"(cosim_full_ram_hash must return all four fields)")
    ok = (_hash_int(full.get("ram0_h")) == _hash_int(full.get("ram0_h_inc")) and
          _hash_int(full.get("ram1_h")) == _hash_int(full.get("ram1_h_inc")))
    return ok, ("full recompute matches incremental at the same instant" if ok else
                "full recompute DIVERGED from incremental at the same instant — "
                "a write-hook site is missing (COSIM-SPEC.md §3b)")


def cmd_gate4(args):
    n = max(1, args.intervals)
    checkpoints = sorted(set(max(1, args.seq * k // n) for k in range(1, n + 1)))
    print(f"[gate4] hash-vs-byte audit: {args.exe}, rom={args.rom}, checkpoints={checkpoints}")
    all_ok = True
    for k, cp in enumerate(checkpoints):
        port = args.base_port + k
        proc = _run_hold(args.exe, args.rom, port, cp, launch_timeout=args.timeout)
        try:
            _wait_for_blocks(port, cp, args.timeout)
            tc = TraceClient(port)
            try:
                full = tc.full_ram_hash()
            finally:
                tc.close()
        finally:
            _stop(proc, port)

        ok, reason = _gate4_verify(full, min_seq=cp)
        f = full or {}
        print(f"[gate4]   seq {f.get('seq', cp)}: full ram0_h={f.get('ram0_h')} ram1_h={f.get('ram1_h')} "
              f"vs incremental (same response) ram0_h_inc={f.get('ram0_h_inc')} "
              f"ram1_h_inc={f.get('ram1_h_inc')} -> {'PASS' if ok else 'FAIL'} ({reason})")
        all_ok = all_ok and ok

    verdict = ("dirty-page tracking matches full recompute at every checkpoint (same-instant compare)"
               if all_ok else
               "DIVERGED from full recompute — a write-hook site is missing (COSIM-SPEC.md §3b)")
    print(f"[gate4] {'PASS' if all_ok else 'FAIL'}: {verdict}")
    return 0 if all_ok else 1


# --------------------------------------------------------------------------
# gates — run 1..4 in order, stop on first FAIL (§7)
# --------------------------------------------------------------------------
def cmd_gates(args):
    results = []

    def run(name, fn, ns):
        print(f"\n===== {name} =====", flush=True)
        try:
            rc = fn(ns)
        except RuntimeError as e:
            print(f"[{name}] FAIL: {e}", file=sys.stderr)
            rc = 1
        results.append((name, rc))
        return rc

    seq = args.seq
    rc = run("gate1 (oracle self-determinism)", cmd_gate1,
              argparse.Namespace(oracle_exe=args.oracle_exe, rom=args.rom, seq=seq, base_port=25380))

    if rc == 0:
        rc = run("gate2 (native self-determinism)", cmd_gate2,
                  argparse.Namespace(native_exe=args.native_exe, rom=args.rom, seq=seq, base_port=25390))

    if rc == 0:
        # Exercise BOTH injection kinds (ram bank localization AND register
        # localization) on BOTH executables — the complete pass, not a single
        # spot-check (CLAUDE.md: no narrower-scope substitute for a general one).
        injections = [("ram", 0x1000, 0xff), ("reg", 0, 0x1)]
        inj_seq = max(1, seq // 4)
        for label, exe, base in (("native", args.native_exe, 25400), ("oracle", args.oracle_exe, 25420)):
            for kind, idx, xorhex in injections:
                if rc != 0:
                    break
                rc = run(f"gate3 ({label}, {kind} injection)", cmd_gate3, argparse.Namespace(
                    exe=exe, rom=args.rom, seq=inj_seq, kind=kind, idx=idx, xorhex=xorhex,
                    margin=200, base_port=base, drift=1, tolerance=0))
            if rc != 0:
                break

    if rc == 0:
        for label, exe, base in (("native", args.native_exe, 25440), ("oracle", args.oracle_exe, 25450)):
            rc = run(f"gate4 ({label})", cmd_gate4,
                      argparse.Namespace(exe=exe, rom=args.rom, seq=seq, intervals=4,
                                         base_port=base, timeout=60.0))
            if rc != 0:
                break

    print("\n===== SUMMARY =====")
    for name, rc in results:
        print(f"  {'PASS' if rc == 0 else 'FAIL'}  {name}")
    overall = 0 if results and all(rc == 0 for _, rc in results) else 1
    if overall == 0:
        print("\noverall: ALL GATES PASS — a diff run can be trusted (COSIM-SPEC.md §7).")
    else:
        print("\noverall: GATES FAILED — do not trust a diff run until these pass.")
    return overall


# --------------------------------------------------------------------------
# --self-test — synthetic record streams, no sockets, no subprocesses.
# --------------------------------------------------------------------------
def _mk_rec(seq, pc=0x00401000, sr=0x2700, d=None, a=None, usp=0x00300000, ssp=0x00310000,
            ram0_h="0000000000000001", ram1_h="0000000000000002", tcyc=0, **overrides):
    d = list(d) if d else [0] * 8
    a = list(a) if a else [0] * 8
    rec = {"seq": seq, "pc": pc, "sr": sr, "usp": usp, "ssp": ssp,
           "ram0_h": ram0_h, "ram1_h": ram1_h, "tcyc": tcyc, "frame": 0, "a7top": 0}
    for i in range(8):
        rec[f"d{i}"] = d[i]
        rec[f"a{i}"] = a[i]
    rec.update(overrides)
    return rec


def _t_identical():
    native = [_mk_rec(s, pc=0x1000 + s * 2, d=[s, 0, 0, 0, 0, 0, 0, 0]) for s in range(1, 60)]
    oracle = [dict(r) for r in native]
    out = diff_core(native, oracle)
    assert out.ok, out.message
    assert out.compared == len(native)
    assert not out.resyncs


def _t_single_field_diff():
    for field_name, override in [("ssp", {"ssp": 0x00310004}), ("usp", {"usp": 0x1}),
                                  ("d3", {"d3": 99}), ("a5", {"a5": 7}),
                                  ("sr", {"sr": 0x2704}), ("pc", {"pc": 0xdead})]:
        native = [_mk_rec(s, pc=0x1000 + s * 2, d=[s, 0, 0, 0, 0, 0, 0, 0]) for s in range(1, 40)]
        oracle = [dict(r) for r in native]
        oracle[20] = dict(oracle[20])
        oracle[20].update(override)
        out = diff_core(native, oracle, drift=3, confirm=2)
        assert not out.ok, f"{field_name}: expected a divergence, got clean"
        assert out.signal == field_name, f"{field_name}: reported signal {out.signal!r} instead"
        assert out.divergence_seq_native == native[20]["seq"]
        assert out.divergence_seq_oracle == oracle[20]["seq"]


def _t_benign_offset_shift():
    # Oracle spins 2 extra (benign) records at seq index 10, then re-converges
    # to the SAME content as native from there on, shifted by +2. A bounded
    # adaptive-offset search (drift>=2) must bridge this with zero false
    # divergence and record exactly one resync of delta=2.
    native = [_mk_rec(s, pc=0x2000 + s * 2, d=[s, 0, 0, 0, 0, 0, 0, 0]) for s in range(1, 40)]
    oracle = []
    oseq = 1
    for i, r in enumerate(native):
        if i == 10:
            oracle.append(_mk_rec(oseq, pc=0x9999, sr=0x2704)); oseq += 1
            oracle.append(_mk_rec(oseq, pc=0x9998, sr=0x2704)); oseq += 1
        nr = dict(r); nr["seq"] = oseq; oseq += 1
        oracle.append(nr)
    out = diff_core(native, oracle, drift=3, confirm=2)
    assert out.ok, f"expected a bridged benign re-sync, got: {out.message}"
    assert len(out.resyncs) == 1, f"expected exactly 1 resync, got {out.resyncs}"
    side, delta, nseq, oseq_ = out.resyncs[0]
    assert side == "oracle" and delta == 2, out.resyncs


def _t_ram_only_divergence():
    # Identical registers on both sides, ram0_h differs — the class the
    # register-only tool was blind to (COSIM-SPEC.md §0/§6). Content must vary
    # per record (pc/d0 march forward) so records aren't coincidentally
    # indistinguishable from each other — otherwise the offset search could
    # spuriously "resync" against an unrelated but identical-looking record.
    native = [_mk_rec(s, pc=0x4000 + s * 2, d=[s, 0, 0, 0, 0, 0, 0, 0]) for s in range(1, 30)]
    oracle = [dict(r) for r in native]
    oracle[9] = dict(oracle[9])
    oracle[9]["ram0_h"] = "ffffffffffffffff"
    out = diff_core(native, oracle)
    assert not out.ok, out.message
    assert out.signal == "ram0_h", out.signal
    assert out.divergence_seq_native == native[9]["seq"]
    # and the symmetric case for bank 1
    native2 = [_mk_rec(s, pc=0x5000 + s * 2, d=[s, 0, 0, 0, 0, 0, 0, 0]) for s in range(1, 30)]
    oracle2 = [dict(r) for r in native2]
    oracle2[15] = dict(oracle2[15])
    oracle2[15]["ram1_h"] = "1111111111111111"
    out2 = diff_core(native2, oracle2)
    assert not out2.ok, out2.message
    assert out2.signal == "ram1_h", out2.signal


def _t_tcyc_informational_only():
    native = [_mk_rec(s, tcyc=1000 + s) for s in range(1, 30)]
    oracle = [dict(r) for r in native]
    for r in oracle:
        r["tcyc"] = r["tcyc"] + 5     # constant device-timing drift, nothing else differs
    out = diff_core(native, oracle)
    assert out.ok, f"tcyc-only drift must never fail the diff, got: {out.message}"
    assert len(out.tcyc_drifts) == len(native), "tcyc drift should be reported at every aligned seq"


def _t_injection_signal_mapping():
    assert _expected_signal_for_injection("reg", 0) == "d0"
    assert _expected_signal_for_injection("reg", 7) == "d7"
    assert _expected_signal_for_injection("reg", 8) == "a0"
    assert _expected_signal_for_injection("reg", 15) == "a7"
    assert _expected_signal_for_injection("reg", 16) == "usp"
    assert _expected_signal_for_injection("reg", 17) == "ssp"
    assert _expected_signal_for_injection("ram", 0x0) == "ram0_h"
    assert _expected_signal_for_injection("ram", 0x0007FFFF) == "ram0_h"
    assert _expected_signal_for_injection("ram", 0x00200000) == "ram1_h"
    assert _expected_signal_for_injection("ram", 0x0027FFFF) == "ram1_h"
    for bad in [("reg", 18), ("reg", -1), ("ram", 0x00080000), ("ram", 0x001FFFFF)]:
        try:
            _expected_signal_for_injection(*bad)
            raise AssertionError(f"expected ValueError for {bad}")
        except ValueError:
            pass


def _t_hash_representation_tolerant():
    assert _hash_int("00000000000000ff") == 255
    assert _hash_int(255) == 255
    a = _mk_rec(1, ram0_h="00000000000000ff")
    b = _mk_rec(1, ram0_h=255)
    assert full_state(a) == full_state(b)


def _t_gate4_same_response_check():
    # §5: matching full-recompute vs incremental fields in the SAME response
    # (same instant) -> PASS.
    ok, _ = _gate4_verify({"ok": True, "seq": 5,
                            "ram0_h": "00000000000000ff", "ram1_h": "0000000000000002",
                            "ram0_h_inc": "00000000000000ff", "ram1_h_inc": "0000000000000002"})
    assert ok
    # A missed write-hook site shows up as full != incremental within that one
    # response -- no ring record / --stop-seq skew involved at all.
    ok2, reason2 = _gate4_verify({"ok": True, "seq": 5,
                                   "ram0_h": "00000000000000ff", "ram1_h": "0000000000000002",
                                   "ram0_h_inc": "00000000000000aa", "ram1_h_inc": "0000000000000002"})
    assert not ok2, reason2
    # An older server (pre-§5 four-field response) must FAIL loudly, not
    # silently pass or crash.
    ok3, _ = _gate4_verify({"ok": True, "seq": 5, "ram0_h": "1", "ram1_h": "2"})
    assert not ok3
    ok4, _ = _gate4_verify(None)
    assert not ok4
    ok5, _ = _gate4_verify({"ok": False})
    assert not ok5
    # A live native server may accept queries before --stop-seq has parked it.
    # Same-instant hashes from that early state are valid values but not valid
    # evidence for the requested checkpoint.
    early = {"ok": True, "seq": 4,
             "ram0_h": "1", "ram1_h": "2",
             "ram0_h_inc": "1", "ram1_h_inc": "2"}
    ok6, reason6 = _gate4_verify(early, min_seq=5)
    assert not ok6 and "before requested checkpoint" in reason6
    ok7, reason7 = _gate4_verify(dict(early, seq=5), min_seq=5)
    assert ok7, reason7


def _t_real_divergence_not_masked_by_search():
    # A true control-flow/data split: nothing within +/-drift ever restores a
    # full-state match (unlike the benign-shift case). Must NOT be bridged.
    native = [_mk_rec(s, pc=0x3000 + s * 2, d=[s, 0, 0, 0, 0, 0, 0, 0]) for s in range(1, 40)]
    oracle = [dict(r) for r in native]
    for k in range(15, len(oracle)):
        oracle[k] = dict(oracle[k])
        oracle[k]["d0"] = oracle[k]["d0"] + 1000   # permanently different from here on
    out = diff_core(native, oracle, drift=3, confirm=2)
    assert not out.ok
    assert out.signal == "d0"
    assert out.divergence_seq_native == native[15]["seq"]


def _t_cycle_cost_attribution_and_grouping():
    native = [
        {"seq": 1, "pc": 0x1000, "op": 0x4E71, "tcyc": 10},
        {"seq": 2, "pc": 0x1002, "op": 0x7000, "tcyc": 17},
        {"seq": 3, "pc": 0x1004, "op": 0x4E75, "tcyc": 25},
    ]
    oracle = [
        {"seq": 1, "pc": 0x1000, "op": 0x4E71, "tcyc": 100},
        {"seq": 2, "pc": 0x1002, "op": 0x7000, "tcyc": 107},
        {"seq": 3, "pc": 0x1004, "op": 0x4E75, "tcyc": 114},
    ]
    audit = CycleAudit()
    for i in range(1, len(native)):
        _cycle_audit_add(audit, native[i - 1], oracle[i - 1], native[i], oracle[i])
    assert audit.transitions == 2 and audit.aligned == 2 and audit.skipped == 0
    assert audit.mismatches == 1 and audit.contribution == 1
    assert audit.first == (2, 2, 0x1002, 0x7000, 8, 7)
    assert audit.groups[(0x1002, 0x7000)].pairs[(8, 7)] == 1


def _self_test():
    tests = [
        ("identical streams => no divergence", _t_identical),
        ("single CPU-field diff => halts naming that exact field", _t_single_field_diff),
        ("benign constant offset shift => bridged, zero false divergence", _t_benign_offset_shift),
        ("ram0_h/ram1_h-only diff (same regs) => flagged as ram*_h divergence", _t_ram_only_divergence),
        ("tcyc drift alone is informational, never fails the diff", _t_tcyc_informational_only),
        ("--cosim-inject idx -> signal-name mapping (gate3's predictor)", _t_injection_signal_mapping),
        ("ram*_h hex-string vs int representations compare equal", _t_hash_representation_tolerant),
        ("gate4 same-response full-vs-incremental check (§5)", _t_gate4_same_response_check),
        ("a real, non-bridgeable split is never masked by the offset search", _t_real_divergence_not_masked_by_search),
        ("cycle deltas attribute to record N and aggregate by PC/opcode", _t_cycle_cost_attribution_and_grouping),
    ]
    n_fail = 0
    for name, fn in tests:
        try:
            fn()
            print(f"  PASS  {name}")
        except AssertionError as e:
            n_fail += 1
            print(f"  FAIL  {name}: {e}")
        except Exception as e:
            n_fail += 1
            print(f"  ERROR {name}: {type(e).__name__}: {e}")
    print(f"\n{len(tests) - n_fail}/{len(tests)} self-tests passed.")
    return 0 if n_fail == 0 else 1


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(prog="cdi_cosim.py", description="COSIM-SPEC.md coordinator")
    ap.add_argument("--self-test", action="store_true",
                     help="run synthetic self-tests (no sockets/subprocesses) and exit")
    sub = ap.add_subparsers(dest="command")

    p_cyc = sub.add_parser("cycles", help="aggregate aligned per-instruction cycle-cost mismatches")
    p_cyc.add_argument("native_port", type=int)
    p_cyc.add_argument("oracle_port", type=int)
    p_cyc.add_argument("--start", type=int, default=1,
                       help="first seq to audit (default 1 skips reset seam)")
    p_cyc.add_argument("--stop", type=int, default=None,
                       help="exclusive record stop; priced interval ends one seq earlier")
    p_cyc.add_argument("--top", type=int, default=30,
                       help="number of PC/opcode groups to report")
    p_cyc.add_argument("--progress", type=int, default=100000,
                       help="progress interval in seqs; 0 disables")
    p_cyc.add_argument("--window", type=int, default=4096,
                       help="lookahead records for PC/opcode re-alignment")
    p_cyc.add_argument("--confirm", type=int, default=2,
                       help="matching instructions required at a re-sync target")
    p_cyc.set_defaults(func=cmd_cycles)

    p_cr = sub.add_parser("cycle-run", help="free-run, park, and audit both sides")
    p_cr.add_argument("native_exe")
    p_cr.add_argument("oracle_exe")
    p_cr.add_argument("rom")
    p_cr.add_argument("seq", nargs="?", type=int, default=530001,
                      help="records to capture; one beyond the final priced instruction")
    p_cr.add_argument("--start", type=int, default=1)
    p_cr.add_argument("--top", type=int, default=30)
    p_cr.add_argument("--progress", type=int, default=100000)
    p_cr.add_argument("--window", type=int, default=4096)
    p_cr.add_argument("--confirm", type=int, default=2)
    p_cr.add_argument("--native-port", type=int, default=25500)
    p_cr.add_argument("--oracle-port", type=int, default=25501)
    p_cr.add_argument("--timeout", type=float, default=180.0)
    p_cr.set_defaults(func=cmd_cycle_run)

    p_diff = sub.add_parser("diff", help="align+diff two already-running, held sides")
    p_diff.add_argument("native_port", type=int)
    p_diff.add_argument("oracle_port", type=int)
    p_diff.add_argument("--start", type=int, default=1,
                         help="first seq to compare (default 1 skips the seq-0 reset seam)")
    p_diff.add_argument("--drift", type=int, default=3, help="bounded adaptive-offset search radius")
    p_diff.add_argument("--confirm", type=int, default=2,
                         help="following records required to confirm a candidate resync")
    p_diff.add_argument("--context", type=int, default=6, help="rows of context each side")
    p_diff.add_argument("--limit", type=int, default=None, help="cap records loaded per side")
    p_diff.set_defaults(func=cmd_diff)

    p_g1 = sub.add_parser("gate1", help="oracle self-determinism")
    p_g1.add_argument("oracle_exe")
    p_g1.add_argument("rom")
    p_g1.add_argument("seq", nargs="?", type=int, default=20000)
    p_g1.add_argument("--base-port", type=int, default=25380)
    p_g1.set_defaults(func=cmd_gate1)

    p_g2 = sub.add_parser("gate2", help="native self-determinism")
    p_g2.add_argument("native_exe")
    p_g2.add_argument("rom")
    p_g2.add_argument("seq", nargs="?", type=int, default=20000)
    p_g2.add_argument("--base-port", type=int, default=25390)
    p_g2.set_defaults(func=cmd_gate2)

    p_g3 = sub.add_parser("gate3", help="fault-injection localizes")
    p_g3.add_argument("exe")
    p_g3.add_argument("rom")
    p_g3.add_argument("seq", type=int)
    p_g3.add_argument("kind", choices=["ram", "reg"])
    p_g3.add_argument("idx", type=lambda x: int(x, 0))
    p_g3.add_argument("xorhex", type=lambda x: int(x, 0))
    p_g3.add_argument("--margin", type=int, default=200, help="extra seq past the injection to run/verify")
    p_g3.add_argument("--base-port", type=int, default=25400)
    p_g3.add_argument("--drift", type=int, default=1, help="offset search radius for the baseline-vs-injected diff")
    p_g3.add_argument("--tolerance", type=int, default=0, help="allowed seq slack between injection and localized divergence")
    p_g3.set_defaults(func=cmd_gate3)

    p_g4 = sub.add_parser("gate4", help="hash-vs-byte audit")
    p_g4.add_argument("exe")
    p_g4.add_argument("rom")
    p_g4.add_argument("seq", nargs="?", type=int, default=20000)
    p_g4.add_argument("--intervals", type=int, default=4)
    p_g4.add_argument("--base-port", type=int, default=25410)
    p_g4.add_argument("--timeout", type=float, default=60.0)
    p_g4.set_defaults(func=cmd_gate4)

    p_gs = sub.add_parser("gates", help="run gate1..gate4 in order, stop on first FAIL")
    p_gs.add_argument("native_exe")
    p_gs.add_argument("oracle_exe")
    p_gs.add_argument("rom")
    p_gs.add_argument("seq", nargs="?", type=int, default=20000)
    p_gs.set_defaults(func=cmd_gates)

    args = ap.parse_args()

    if args.self_test:
        return _self_test()

    if not getattr(args, "func", None):
        ap.print_help()
        return 2

    try:
        return args.func(args)
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
