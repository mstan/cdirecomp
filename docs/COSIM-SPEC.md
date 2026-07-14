# COSIM-SPEC.md — cdirecomp differential co-simulation wire contract

**Status:** design contract for MC-CDI-016 (full-state divergence instrument).
**Audience:** the native-side (C), oracle-side (C++), and coordinator (Python)
implementers. This file is the SINGLE source of truth for the wire format. The
native runner (`CdiRuntime`, C) and the oracle (`CdiOracle`, CeDImu/C++) share no
translation unit, so the hash is written twice by hand — it MUST be byte-identical.
Everything a cross-side hash depends on is pinned here; if you change any of it,
change this file first and both sides together.

## 0. What this is (and what it replaces)

An always-on, ring-buffer differential co-sim between the recompiled CD-RTOS
(native, TCP :4396/:4380) and the CeDImu oracle (TCP :4381). It UPGRADES the
existing register-only `tools/realign_divergence.py` from comparing register
tuples to comparing **full architectural state (CPU incl. USP/SSP + guest RAM)**
per retired instruction, so the "same registers, different memory" divergence
class (and the inactive-stack-pointer class, e.g. the current SSP frontier at
seq ~562636) stops being invisible.

**Doctrine (non-negotiable).** Both sides FREE-RUN from boot with always-on
rings; the coordinator QUERIES the rings and diffs. There is NO "park both,
step N, compare" lockstep control plane — that is the pause/step-two-observers
anti-pattern (see CLAUDE.md / PRINCIPLES.md ring-buffer rule). The only
"stop" primitive is the existing per-side `--stop-seq` freeze (run ONE side to
a deterministic seq and hold with rings live for drill-down); it is never used
to synchronize the two observers.

## 1. Alignment clock

- **Alignment key = `seq`** (retired-instruction count, the ring record index).
  Both sides retire identical guest instructions in identical order up to the
  first divergence, so seq N = the same guest instruction on both — the ideal
  alignment clock. The coordinator aligns on seq with a bounded adaptive offset
  to bridge benign timing seams (see §6).
- **`tcyc` (cycle counter) is INFORMATIONAL ONLY.** It has a known ~1-frame
  sub-frame drift; it MUST NOT drive alignment and MUST NOT be folded into the
  state hash. A `tcyc` divergence at otherwise-matching seq-aligned state is a
  separate device-timing finding, reported, never a state-correctness fail.

## 2. State surface (what is compared)

The reliable cross-implementation surface between two DIFFERENT emulator cores is
**CPU registers + guest RAM only**. Do NOT cross-hash device internals: native's
MCD212/CDIC/IKAT/periph models and CeDImu's represent the same guest-visible
state with different internal layouts, so a device sub-hash would false-positive
even when faithful. Device divergences surface downstream in RAM/registers.

### 2a. CPU (compared as explicit fields, NOT hashed)

Compared field-by-field for full transparency (the state is tiny). Fields, all
already present on both sides:

| field | native source | oracle source |
|---|---|---|
| `d0..d7` | `g_cpu.D[0..7]` | `GetCPURegisters()[Reg::D0..D7]` |
| `a0..a7` | `g_cpu.A[0..7]` | `GetCPURegisters()[Reg::A0..A7]` |
| `sr` | `g_cpu.SR` (16-bit) | `[Reg::SR]` |
| `pc` | `g_cpu.PC` | `currentPC` |
| `usp` | `g_cpu.USP` | `[Reg::USP]` |  ← NEW in both trace JSONs
| `ssp` | `g_cpu.SSP` | `[Reg::SSP]` |  ← NEW in both trace JSONs

`a7` aliases the active stack pointer (SSP when S=1, USP when S=0); `usp`/`ssp`
are the two shadows. The current tooling captures only `a7`, so the INACTIVE
shadow's divergence is invisible — that is the whole SSP-frontier blind spot.

**CANONICALIZATION (critical for cross-side parity).** Emit `usp`/`ssp` in the
CANONICAL form CeDImu uses, on BOTH sides:
```
canonical_ssp = (SR & 0x2000) ? A7 : saved_supervisor_SP     (0x2000 = S bit)
canonical_usp = (SR & 0x2000) ? saved_user_SP : A7
```
- The ORACLE is already canonical: CeDImu's `A(7)` aliases the `SSP` field when
  S=1, so `GetCPURegisters()[Reg::SSP]` is the live super SP in supervisor mode
  and the saved super SP in user mode (same for USP). Emit `Reg::USP`/`Reg::SSP`
  as-is.
- The NATIVE side stores RAW `M68KState` shadows: when S=1, `g_cpu.SSP` is STALE
  (the live super SP is in `A[7]`; the shadow froze at the last swap), and
  symmetrically `g_cpu.USP` is stale when S=0. So the native emit MUST
  canonicalize: `emit_ssp = (SR&0x2000) ? A[7] : g_cpu.SSP`,
  `emit_usp = (SR&0x2000) ? g_cpu.USP : A[7]`. Emitting the raw shadow would make
  `ssp` false-diverge against the oracle throughout supervisor execution.
- `cdi_cosim_inject_reg(16=USP,17=SSP)` must likewise target the CANONICAL
  location so an injected shadow is visible in the emitted field: write `A[7]`
  when that shadow is currently active (S=1 for SSP, S=0 for USP), else the
  `g_cpu.USP`/`.SSP` field. (The oracle already does this via the A7-alias.)

NOTE: native already stores the full `M68KState` (incl. USP/SSP) in each ring
record; it just doesn't emit them. The oracle's `TraceRec` must ADD `usp`/`ssp`.

### 2b. Guest RAM (compared as two incremental FNV-1a hashes)

Two banks, hashed separately for bank-level localization. Ranges are byte-
identical on both sides (verified: native `g_ram0`/`g_ram1`; CeDImu
`GetRAMBank1()`/`GetRAMBank2()` → base/size below):

| hash field | guest range | size | native backing | oracle backing |
|---|---|---|---|---|
| `ram0_h` | `0x00000000..0x0007FFFF` | `0x80000` (512 KB) | `g_ram0[]` | `GetRAMBank1().data` (base 0) |
| `ram1_h` | `0x00200000..0x0027FFFF` | `0x80000` (512 KB) | `g_ram1[]` | `GetRAMBank2().data` (base 0x200000) |

ROM is excluded (static after load, never written → no divergence signal).
MMIO/device windows are excluded (read side effects, non-architectural,
different internal representations).

## 3. Hash algorithm (MUST be byte-identical on both sides)

FNV-1a, 64-bit. Constants (verbatim, same as the sibling projects):

```
FNV_OFFSET_BASIS = 0x14650FB0739D0383   (1469598103934665603)
FNV_PRIME        = 0x00000100000001B3   (1099511628211)

fnv1a(h, bytes[n]):            # h starts at FNV_OFFSET_BASIS
    for each byte b in bytes:
        h = (h XOR b) * FNV_PRIME   (mod 2^64, unsigned wraparound)
    return h
```

### 3a. Incremental RAM page-hash

Each bank is divided into fixed 4096-byte pages (bank size 0x80000 → **128 pages
per bank**). Maintain, per bank:
- `page_hash[128]` : `uint64_t`, `page_hash[p] = fnv1a(BASIS, bank[p*4096 .. p*4096+4095])`
- `page_dirty[128]`: `uint8_t`, all set at reset (forces one full hash), set on write.

On every guest RAM write (see §3b for the hook), mark `page_dirty[addr/4096] = 1`
for each page the write touches (a 32-bit write may straddle two pages).

At capture time (once per seq), recompute the hash of each dirty page, clear its
dirty flag, then fold the whole page-hash array into the bank hash:
```
for p in 0..127: if page_dirty[p]: page_hash[p] = fnv1a(BASIS, bank + p*4096, 4096); page_dirty[p]=0
bank_hash = fnv1a(BASIS, (bytes of) page_hash[0..127])   # hash-of-hashes, 128*8 = 1024 bytes
```
The array is folded in index order 0→127 as little-endian `uint64_t`s. Because
both sides fold the SAME page-hash array the same way, and both derive the array
from byte-identical guest RAM, `ram0_h`/`ram1_h` match pre-divergence.
`ram0_h` uses bank 0's array; `ram1_h` uses bank 1's array.

Cost: O(pages dirtied since last capture) + one 1 KB fold per bank per seq.

### 3b. RAM write hooks (audit every RAM-mutating path)

The incremental hash is only correct if EVERY guest RAM byte mutation marks its
page dirty. Hook the single choke point on each side:
- **Native:** inside the RAM branch (`if (p) { ... }`) of `m68k_write8` and
  `m68k_write16` in `cdi_bus.c` (the point AFTER `ram_ptr(addr)` confirms RAM —
  NOT `debug_trace_store`, which also logs MMIO). `m68k_write32` decomposes into
  two `m68k_write16`, so hooking write16 covers it. **AUDIT:** confirm no device
  (CDIC/CIAP sector delivery, MCD212 DMA) writes `g_ram0`/`g_ram1` bytes directly
  rather than through `m68k_write*`. If one does, it needs its own dirty-mark
  call. Gate 4 (§7) is the backstop that catches a missed site.
- **Oracle:** the CeDImu RAM write choke point (`Mono3::SetByte` RAM branch, or
  the board's central byte-write). Add a minimal `#ifdef CDI_COSIM` dirty-mark
  hook there, mirroring native. Keep the vendored-tree edit to one call site.

## 4. Ring record additions (both sides, identical JSON field names)

Extend BOTH per-instruction trace rings and their `trace` JSON output. New fields
appended to each record's existing `{seq,pc,sr,d0..d7,a0..a7,a7top,frame,tcyc}`:

```
"usp":  <uint32>,   "ssp":  <uint32>,
"ram0_h": "<16-hex>", "ram1_h": "<16-hex>"     # 64-bit hashes as lowercase hex strings
```

64-bit hashes are emitted as 16-char lowercase hex strings (not JSON numbers —
avoids float precision loss). The coordinator parses them as `int(x, 16)`.

## 5. TCP commands (both :4380 native and :4381 oracle, symmetric)

Add to each side's existing JSON line protocol (`{"cmd":"..."}` → one-line JSON):

```
{"cmd":"cosim_full_ram_hash"}
    -> {"ok":true,"seq":N,
        "ram0_h":"<hex>","ram1_h":"<hex>",           # FULL recompute over live RAM
        "ram0_h_inc":"<hex>","ram1_h_inc":"<hex>"}   # INCREMENTAL at this same instant
    ram*_h = cdi_cosim_full_ram_hash (recompute ALL pages from scratch).
    ram*_h_inc = cdi_cosim_ram_hash (refresh dirty pages, fold) — the incremental
    value AT THE CURRENT PARKED MOMENT. Gate 4 asserts ram*_h == ram*_h_inc from
    the SAME response (same instant), so it is immune to the one-instruction skew
    between the parked machine state (pre-seq-N) and the last captured ring record
    (pre-seq-(N-1)). A mismatch means a missed write-hook site (§3b).
```

The existing `trace {from,count}` command (paged, 256/call) now carries the new
fields — the coordinator reads the hash streams through it; no new streaming
command is needed. `--stop-seq N` (native, and NEW on the oracle) freezes one
side at seq N with rings live for drill-down (`get_registers`/`read_mem`).

For MC-CDI-009 cycle-model audits, both sides also expose a compact view of the
same always-on ring (it does not arm or capture anything):

```
{"cmd":"cycle_trace","from":N,"count":512}
    -> {"ok":true,"total":T,"records":[
         {"seq":N,"pc":P,"op":OP,"cycle_seam":S,"tcyc":C}, ...]}
```

`pc`, `op`, and `tcyc` are sampled for the instruction-entry record. Therefore
the cycle cost attributable to record N is `tcyc[N+1] - tcyc[N]`. `op` is also
present in the full `trace` record as an informational field; it is not a
full-state divergence key.

The oracle sets `cycle_seam=1` when CeDImu processed a queued exception before
the captured instruction (`pre.PC != currentPC`). That `Run(false)` bundles the
exception cost with the handler's first instruction. The coordinator excludes
the transitions on both sides of such a record from opcode-cost aggregation;
they remain visible in the ring for separate exception-timing analysis.

Fault injection for Gate 3 is a startup CLI/env knob, NOT a mid-run command
(keeps the free-run doctrine — the flip is applied when the run reaches the seq):
```
--cosim-inject "<seq>:<kind>:<idx>:<xorhex>"        (also env CDI_COSIM_INJECT)
    kind = "ram"  -> at seq, XOR byte at guest addr <idx> by <xor> (persists in RAM)
    kind = "reg"  -> at seq, XOR register <idx> by <xor>, where idx:
                     0..7 = D0..D7, 8..15 = A0..A7, 16 = USP, 17 = SSP
    Applied to LIVE state (so the hasher must read real state to see it).
```

## 6. Coordinator (`tools/cdi_cosim.py`, or extend `realign_divergence.py`)

- Launch native + oracle, each `--stop-seq`/`--steps` past the window, `--hold`.
- Pull both trace rings via paged `trace {from,count:256}`.
- **Compare per seq:** CPU fields (d0..d7,a0..a7,sr,pc,usp,ssp) exactly + ram0_h
  + ram1_h. Add `usp`,`ssp` to the realign REG_KEYS; add `ram0_h`,`ram1_h` as
  additional divergence signals (a RAM-hash mismatch at an aligned seq with
  matching registers = a real memory divergence the old tool was blind to).
- **Bounded adaptive offset** (from nesrecomp): when a benign timing seam shifts
  the seq offset (e.g. the IRQ take firing a few seq apart), search ±drift around
  the current offset for the offset that restores a full-state (CPU+RAM) match;
  a shift that recovers a match is benign (record it), a match that stays broken
  is the real first divergence. Full-state match (incl. RAM) makes this classify
  more reliably than the register-only tool.
- **Localize:** report the first non-bridgeable divergent seq, WHICH signal split
  (a named CPU field, or ram0/ram1), and a window before it. On a RAM split,
  drill: re-run each side to `--stop-seq <seq>`, `read_mem` the suspect region,
  diff. On a CPU split, the field name is the answer.

## 7. Validation gates (implement all four; trust NOTHING until they pass)

Run in order; each is CI-gateable (exit non-zero on FAIL):

1. **Oracle self-determinism.** Run `CdiOracle` twice, same ROM, same seq window;
   the ram0_h/ram1_h + CPU streams MUST be identical. Catches oracle
   nondeterminism (a wall-clock RTC read, uninitialized state). NVRAM RTC is
   seeded to a fixed epoch today (safe) — re-check if it ever advances from
   host time.
2. **Native self-determinism.** Same for `CdiRuntime`. Catches native
   nondeterminism / uninitialized padding in the hashed surface (memset any
   serialized struct before hashing).
3. **Fault injection localizes.** `--cosim-inject` one byte/register into ONE
   side at seq K; the diff MUST halt at K and name the injected subsystem
   (ram0/ram1 for a RAM flip; the exact register for a reg flip). A RAM flip
   persists/cascades; verify the incremental page-hash actually recomputed the
   touched page. This is the gate that catches a blind/constant hasher — never
   skip it.
4. **Hash-vs-byte audit.** At intervals, first poll `status.blocks` until the
   requested `--stop-seq` checkpoint has actually parked, then call
   `cosim_full_ram_hash`. Require the response `seq` to be at least that
   checkpoint and compare its full and incremental fields from that same
   response. Native exposes TCP while it is still running, so merely waiting
   for the port can otherwise produce a plausible early PASS. A hash mismatch
   means a missed write-hook site (§3b); an early seq means the checkpoint was
   never measured.

Only after 1–4 pass is a native-vs-oracle first-divergence report trusted.

## 8. Anti-blindness safeguards (bake in from day one)

- **Canary:** both hash functions fold a fixed 8-byte magic
  (`0xC0517EC0517EC051`) as the very first bytes of `ram0_h`. A structural
  mismatch (wrong byte order / field count) then shows as "diverges from seq 0",
  an obvious tooling bug, not a plausible-but-wrong "first divergence at seq N".
- **memset before hashing** any struct that has padding.
- **Exclude host-only state** from any serialization: pointers (e.g. slave.c
  `s_delayed_rsp`), jmp_bufs, the debug rings themselves, file/socket handles.
- Treat `runner/src/cosim_state.c` (C) and `oracle/cosim_state.cpp` (C++) as ONE
  logical spec split across two files by necessity — same field order, same
  exclusions, same constants, reviewed together, validated by gates 1–2 before
  any cross-side compare.
