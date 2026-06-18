# RESUME.md — pick up here next session

Single source for resuming the CD-i boot bring-up. Pairs with PLAN.md (the
maturation roadmap) and the `cdi-boot-status` auto-memory. Last updated end of
the 2026-06-18 session (MC-CDI-004 landed).

## Where the boot is

The recompiled CD-RTOS now **boots PAST the `$600000` memory-sizing probe** and
free-runs into a steady **frame-advancing loop around `$405D6x`** (frame counter
climbing, `miss_count 0`, no fault). It is **bit-exact to the CeDImu oracle**
through the entire boot up to the benign DA frame-sync jitter (~seq 66821), and
verified **identical control flow through both bus-error probes** past that.
Cleared walls:

1. `$050A` RAM exception stub → clean-room hybrid interpreter (MC-CDI-011).
2. `$400E96` `BTST #7,$4FFFF1` (MCD212 DA) → display-line timing model.
3. `$400B44` UART TX → USR TxRDY set at reset.
4. `$40069E` deliberate OS-9 address error → faithful exception frame.
5. **`$600000` 68070 bus error (MC-CDI-004)** → faithful vector-2 exception; the
   OS-9 handler at `$000500` services it and boot continues.

## Last two milestones (both DONE this session, in this commit)

### MC-CDI-005 — cycle-accurate timing (prior session, was uncommitted)
Clean-room SCC68070 per-instruction cycle table mirroring CeDImu `calcTime` in
BOTH tiers (`m68k_cycles` in `runner/src/m68k_interp.c`; `estimate_cycles_scc68070`
in `recompiler/src/code_generator.c`) + ns-based MCD212 line timing
(`runner/src/mcd212.c`). The DA-loop divergence at ~seq 66821 turned out to be
benign frame-sync jitter (machines re-converge at a fixed ~12-seq offset).

### MC-CDI-004 — faithful bus error (this session)
The `$600000` read (`MOVE.L (A2),D0` @ `$400790`) is a memory-sizing probe that
MUST raise a 68070 bus error. Implemented entirely in the runner (no
generated-code edits):
- **Mid-instruction unwind**: bus errors fire deep inside `m68k_read*`; the
  interpreter arms a `setjmp`/`longjmp` (`s_bus_env`, saved/restored per step)
  around `exec_one`, mirroring CeDImu's C++ `throw`. `cdi_bus.c:bus_fault` calls
  `m68k_interp_bus_error(addr)` (longjmps when armed, else fail-loud). The catch
  branch builds the frame and lets the loop continue INTO the handler — no
  recursive `call_by_address`.
- **Frame** (`runtime.c`): `m68k_trap_vector` split into `build_exception_frame`
  + `m68k_raise_exception_frame`. Long format-`$F` frame stacks **TPF =
  `g_fault_addr`** (faulting DATA address = CeDImu `lastAddress`); stacked PC =
  `ins.addr + byte_length`. New `g_fault_addr` global; the address-error JMP-odd
  emit sets it too.
- **RTE long frame** (`m68k_interp.c`): pop SR/PC/format, `A7 += 26` for a
  format-`$F` frame.
- **`--stop-seq N`** (`debug_server.c` `g_stop_seq`): freeze rings-intact at seq
  N — the deterministic stop for diffing a window the (now non-faulting) boot
  runs past and evicts. (This is the `--max-insns` cap the old backlog wanted.)

## Confirmed: oracle boots to the shell idle `STOP #$2000` @ `$40A3E2`

A 1,000,000-step oracle run rests with `currentPC=$40A3E2`, SR=`$2000`
(supervisor, IPL=0, interrupts enabled) executing the same instruction forever —
a **`STOP #$2000`**, the player-shell idle waiting for an interrupt. So `$40A3E2`
is the goal. (Our runtime's `genesis_stop_until_interrupt` doesn't truly halt yet
— TODO MC-CDI-007 — so when the native reaches STOP the flat-call just unwinds.)

## DONE this session — CLR `-(An)` predecrement bug (recompiler)

The re-aligning diff (`tools/realign_divergence.py`, new) skipped the benign DA
jitter and pinned the true divergence at `$403EF2` = `CLR.L -(A7)`: the
recompiler emitted `m68k_write32(A7,0)` with **no predecrement** (`A7 -= 4`),
corrupting the stack. Root cause: `code_generator.c` MN_CLR passed `rmw=1` to
`emit_ea_store_ex` like NEG/NOT — but CLR has no preceding load to apply the
`-(An)`/`(An)+` side-effect, so it was silently dropped. Fixed to `rmw=0`
(matches CeDImu's write-only `CLR`). Affects ANY code using CLR with predec/
postinc. The boot now matches the oracle bit-exact (modulo jitter) ~6000
instructions further, to **seq 207124**.

## DONE this session — address-error frame TPF (runtime lastAddress)

The address-error frame's TPF was wrong: the recompiler statically emitted the
odd JMP target (`$4006A3`), but CeDImu stacks `lastAddress` — the last DATA EA
touched before the fault (`$000510`, the last vector-stub write) — because its
`GetWord`/`SetWord` throw `AddressError` WITHOUT updating `lastAddress`. The
static recompiler can't know `$510`, so I added runtime `lastAddress` tracking:
`g_last_access_addr` (cdi_bus.c) is set by every `m68k_read/write` (long
accesses collapse to the operand base), and `build_exception_frame` captures it
as TPF before its own pushes clobber it. The address-error frame is now
**byte-identical** to the oracle (verified). (The bus-error frame was already
byte-identical.)

## THE NEXT TASK — native lingers in the `$406xxx` dispatcher (flat-call desync)

The TPF fix did NOT change the boot trajectory — native still returns to `main`
at ~seq 207149 (the flat-call C stack unwinds). The real wall: at `$40468E`
(`RTS` inside shared `func_403EC2`) native and oracle have **identical guest
state** (A7=`$147C`, `mem[$1478]=$40407C` on BOTH) but native's PC is `$406578`
while the oracle's is `$40407C`. Generated code shows native reached
`func_403EC2` via `recomp_push_return(0x406578); recomp_call_func(func_403EC2)`
at `$406574`, i.e. it CALLED the shared function from `$406574` while the oracle
called it from `$404076`. So **native is still circling the `$406xxx` dispatcher
while the oracle has moved into `$40407x`** — the CLR fix advanced the boot but
did not fully fix the dispatcher lingering. `realign_divergence.py` masks this:
the shared `func_403EC2` normalizes registers, so register-alignment matches
native's invocation-from-`$406574` with the oracle's invocation-from-`$404076`,
and only surfaces the symptom (the RTS returning to different callers).

Why the dispatcher lingers is the open question. The `$406262` dispatcher walks
a process/queue list (`[A7]→A0`, `[A0]→A3`, compares A3 against `[A4]`,`[A4+4]`,
`[A4+28]`); some queue/flag it tests differs from the oracle. Given register/PC
execution is otherwise lockstep, the differing datum is in **memory**, written
outside what register-alignment sees. To find it you need either:
- a **memory-write trace** (record (PC, addr, value) per store on both sides and
  diff — the definitive tool; the current realign is register-only), or
- **call-stack-aware alignment** (align by the guest call stack / A7-frame chain,
  not just registers, so a shared callee can't mask a caller divergence), or
- walk back from `$406574` (native) vs `$404076` (oracle) through their callers
  to the first function only one side enters.

Strongly suspect this ties to the **flat-call model**: exceptions
(`m68k_trap_vector` → `call_by_address`) and hybrid-interpreter handoffs push/pop
the GUEST stack via `recomp_push_return` + A7 clamping, which can desync the C
call structure from the real guest stack — so a recompiled `RTS` (C return) lands
somewhere the guest-stack `RTS` would not. The `_sp_popped`/`g_split_sp_popped`
counters at each `RTS` emit are the flat-call's bookkeeping for this; study them.

After that, **interrupt delivery (MC-CDI-007/010)**: `cdi_irq_raise` sets
`g_irq_pending` but nothing consumes it; the shell's `STOP #$2000` @ `$40A3E2`
needs a timer/display IRQ to wake. Vector a pending IRQ above the SR I-mask via
`build_exception_frame` (short autovector frame) between instructions; make
`genesis_stop_until_interrupt` idle until then.

## Validation workflow (READ — non-obvious environment gotchas)

- **Build with the PowerShell tool**, not bash→`powershell.exe` (gcc temp-file
  bug). Set `$env:TMP`/`$env:TEMP` to `F:\Projects\cdirecomp\build\tmp` and use
  the **mingw64 cmake** `C:\msys64\mingw64\bin\cmake.exe`.
- **The oracle is C++**; launching it needs `C:\msys64\mingw64\bin` on PATH for
  its mingw DLLs.
- **Regen after any `code_generator.c` change:**
  `build/recompiler/CdiRecompBios.exe bios/cdi490a.rom --emit`, then rebuild the
  runner. Watch timestamps.
- **Port note:** a sibling project squats TCP 4380; run the native with
  `--port 4390` and diff `--native 4390` (oracle on 4381).

Build + diff, end to end:
```
$cm="C:\msys64\mingw64\bin\cmake.exe"; $env:TMP=$env:TEMP="F:\Projects\cdirecomp\build\tmp"
& $cm --build F:/Projects/cdirecomp/build/recompiler -j
& F:\Projects\cdirecomp\build\recompiler\CdiRecompBios.exe bios/cdi490a.rom --emit
& $cm --build F:/Projects/cdirecomp/build/runner -j

# oracle FIRST (fills its ring), THEN native frozen with rings intact:
Start CdiOracle.exe  bios/cdi490a.rom --steps 250000 --hold          # :4381
Start CdiRuntime.exe bios/cdi490a.rom --stop-seq 131000 --port 4390  # freeze at seq N
python tools/first_divergence.py --native 4390 --oracle 4381 --start 43161 --context 4
python tools/trace_pcs.py --port {4390|4381} --from S --count N      # compact PC/SR list
```

- `--stop-seq N` freezes rings-intact at a chosen seq (use for a boot that no
  longer faults — it would otherwise run past the window and evict it).
  `--fault-hold` still freezes at a fatal fault.
- `first_divergence.py` is **index-aligned** and stops at the benign DA-loop
  jitter (~seq 66821) — past that, confirm progress by matching register state
  across the ~12-seq offset (or diff a specific window with `trace_pcs.py`), NOT
  by the tool's "first divergence". A re-aligning diff (compare by register-state
  hash, not seq index) is still on the backlog.
- `--start 43161` skips the one-seq A7 capture seam at the `$40069E` trap.

## Tactical backlog (smaller, independent)

- **Re-aligning diff:** `first_divergence` is index-aligned and stalls at wait
  loops. A variant that re-aligns after spin loops (or hashes register state)
  would surface the TRUE next divergence instead of the benign DA jitter.
- **MC-CDI-009:** the interp `m68k_cycles` and recompiler
  `estimate_cycles_scc68070` are duplicated — extract the single shared model.
- **Bus-error stacked-PC edge case:** `ins.addr + byte_length` slightly
  overshoots for an instruction that source-reads before fetching trailing
  destination extension words (exact for the register-indirect probe). Replicate
  CeDImu's exact word-consumption-at-throw if a future fault needs it.
- **first_divergence is slow** (a TCP round-trip per page). A `trace_batch`
  command would speed iteration.

## Map of what changed (recent commits)

- `ca51d4d` oracle driver + first_divergence (Phase B)
- `d26437e` entry-based tracing (boot verified bit-exact)
- `373beb7` MC-CDI-011 clean-room hybrid interpreter
- `c2872d1` MCD212 DA timing + UART TxRDY
- `5bee0e3` address-error frame correctness + `--fault-hold`
- *(this commit)* MC-CDI-005 cycle timing + MC-CDI-004 faithful bus error

Key files: `runner/src/m68k_interp.c` (interpreter, `m68k_cycles`, bus-error
unwind, RTE long frame), `runner/src/runtime.c` (`build_exception_frame`,
`g_fault_addr`), `runner/src/cdi_bus.c` (`bus_fault` → interp unwind),
`runner/src/debug_server.c` (`--stop-seq`), `runner/src/mcd212.c` (ns timing),
`recompiler/src/code_generator.c` (`estimate_cycles_scc68070`, odd-JMP emit).
