# Session Handoff — cdirecomp (Philips CD-i SCC68070 → C static recompiler)

**Date:** 2026-06-18
**Phase:** Phase C — boot the recompiled CD-RTOS to the player shell (PLAN.md). Phases A (observability) and B (CeDImu oracle parity) are done.

> Read this handoff + the `cdi-boot-status` auto-memory + `CLAUDE.md`/`PRINCIPLES.md`/`DEBUG.md`. This handoff is authoritative for the most recent work. Everything below is **uncommitted on `master`** unless you commit it.

---

## Headline

The boot advanced from the old wall (**seq 207674**, "returned to main") to **faithful, oracle-validated execution through seq 248748** (no true divergence, 773 benign re-syncs), and now runs to **seq 276326** before hitting a *new* wall: a `$600000` memory-sizing probe **read from the recompiled tier** that the recompiled bus path can't yet turn into a faithful vector-2 bus error.

Three things got us here, in order: a context-switch trampoline (architectural), then two real recompiler codegen bug fixes (PEA / MOVE), plus two observability tools (stack-top in the diff key, and an always-on store ring).

---

## What was done this session

### 1. Tooling — `mem[A7]` in the realign key + writing-loop detector
- `runner/src/debug_server.c`: `CdiTraceRecord` gained `uint32_t a7_top`, captured per block by new `debug_peek_be32(g_cpu.A[7])` (side-effect-free `debug_peek8`, **not** `m68k_read` — must not perturb `g_last_access_addr`). Emitted as `"a7top"` in the `trace` JSON. Per-record JSON margin bumped 320→400.
- `oracle/cdi_oracle.cpp`: `TraceRec` gained `a7top`, filled via new `peek_be32` over CeDImu `GetPointer`; emitted in JSON.
- `tools/realign_divergence.py`: `"a7top"` appended to `REG_KEYS` (a caller/context-switch split with identical regs but a different pushed return can no longer hide); new `addr_advances()` flags a one-sided skip whose skipped records **march an address register A0–A6** (fill/copy loop = real write-count divergence) vs an in-place poll loop (benign). `fmt()` prints `[A7]=`. Gated by `--no-split-detect`. Synthetic tests pass; on real data folds in with no regression.

### 2. Tooling — always-on store ring (chase memory-write divergences to the writer)
- `runner/src/debug_server.c`: `CdiStoreRecord` ring (`1<<20` entries) + `debug_trace_store(addr,val,size)` (records block seq, `g_cpu.PC`, addr, val, size). New `stores` TCP command: params `{addr (required), before (seq upper bound), count}` → most-recent covering writes, oldest-first.
- `runner/include/debug_server.h`: `debug_trace_store` declared.
- `runner/src/cdi_bus.c`: hooks in `m68k_write8/16/32` (a `s_store_suppress` guard makes a 32-bit write log one 4-byte record, not two 16-bit ones).
- `build/tmp/q.py`: scratch helper to query the `stores` command (not load-bearing).

### 3. MC-CDI-012 — context-switch trampoline (ARCHITECTURAL; the 207674 wall)
The flat-call model returns control up the **C** stack on RTS, but OS-9 boot transitions to the shell via the dispatcher and does NOT bottom out at the reset entry. Two fixes:
- **JSR-site redirect** (`recompiler/src/code_generator.c`, `MN_BSR`/`MN_JSR`): when the callee subtree rewrote the stacked return (`[A7] != ret_addr`), set globals `g_redirect_addr`/`g_redirect_pending` and `return` (instead of the old local `recomp_tail_call`). A new `if (g_redirect_pending) return;` at the top of every JSR site propagates it **uncleared** up every C frame. `recomp_drain_tailcalls` also breaks on it.
- **Top-level trampoline** (`runner/src/main.c`): after `recomp_call_addr(reset_pc)` returns, loop — if `g_redirect_pending`, dispatch `g_redirect_addr`; else if `g_halted` (STOP) break; else if the guest stack unwound to `g_recomp_initial_ssp` break; **else follow the guest stack: pop `[A7]` and dispatch it.** That last case fixes a recompiled RTS that bottoms out to `main` with the guest stack still holding a return (the `$406354` RTS → `$4040F0` wall: the function was entered via the dispatcher/exception path, so no JSR site existed to pop `[A7]`). No-progress guard fails loud.
- Globals: `runner/src/runtime.c` defines `g_redirect_pending`, `g_redirect_addr`, `g_halted`; `genesis_stop_until_interrupt` now sets `g_halted=1`. Declared in `runner/include/cdi_runtime.h`.

### 4. Two real recompiler codegen bug fixes (root-caused via the store ring + CeDImu oracle)
- **`MN_PEA`** (`code_generator.c`): `PEA (A7)` (opcode `$4857`) emitted `A7 -= 4; write32(A7, A7)` — it predecremented A7 then re-evaluated the EA as the **decremented** A7 (off-by-4). Fixed to capture the EA into a temp before the push: `{ uint32_t _ea = <addr>; A7 -= 4; write32(A7, _ea); }`. This was the **direct cause of the 210240 divergence** (`PEA (A7)` at `$406568` stored `$14E8` instead of `$14EC`; read back at `$404110`).
- **`MN_MOVE`** (`code_generator.c`): register-direct sources were returned as a lazy `g_cpu.A[reg]` expression evaluated at store time, so `MOVE An,-(An)` stored the post-decrement value. Fixed to materialize register-direct (`Dn`/`An`) sources into the temp before the store. (Latent bug; CeDImu `InstructionSet.cpp::MOVE` reads `src` fully before `SetLong`.)

### Files modified
```
recompiler/src/code_generator.c      (JSR redirect+propagate, drain break, MN_PEA, MN_MOVE)
runner/src/runtime.c                 (g_redirect_pending/addr, g_halted, STOP sets g_halted)
runner/src/main.c                    (top-level guest-stack trampoline)
runner/include/cdi_runtime.h         (g_redirect_pending/addr, g_halted externs)
runner/src/debug_server.c            (a7_top capture, debug_peek_be32, store ring, resp_stores, 'stores' cmd)
runner/include/debug_server.h        (debug_trace_store decl)
runner/src/cdi_bus.c                 (store-ring hooks in m68k_write8/16/32)
oracle/cdi_oracle.cpp                (a7top in TraceRec + capture + JSON)
tools/realign_divergence.py          (a7top key, addr_advances writing-loop detector, fmt [A7])
bios/generated/cdrtos_{full,dispatch}.c   (REGENERATED after every code_generator.c change)
```

---

## Verified state

- **Builds clean** (mingw64 cmake): `CdiRecomp`, `CdiRecompBios`, regenerated BIOS, `CdiRuntime`, `CdiOracle`.
- **Faithful boot:** `realign_divergence.py --start 1 --window 8192` over `[1, 248748)` → **no true divergence**, 773 benign re-syncs, max spin skew 10. (Old wall was 207674.)
- **The `$406354` RTS now follows the guest stack to `$4040F0`** (was returning to `main`); verified in the trace (native seq 207673 `$406354` → 207674 `$4040F0` → … → `$406330`, matching the oracle).
- **Codegen fixes confirmed:** 0 remaining `A[n] -= 4; write(A[n], A[n])` same-register sites in generated output.
- **a7top** present in both trace streams; folds into the realign key with no regression/false positive.
- **Synthetic detector tests pass** (fill-loop flagged, poll/single-step benign, a7top in key).
- **dispatch_miss count = 0** on this path.
- **New wall:** native standalone aborts at seq **276326** with `[bus] UNMAPPED R16 @ $00600000 (PC=$00404B84)`.

---

## Current runtime state

- ROM: `bios/cdi490a.rom` (512 KB, Mono-IV / 32 KB NVRAM / NTSC). Reset SSP=$1500, PC=$4004B8, SR=$2700.
- Static dispatch table: **348** recompiled functions (unchanged this session).
- Active machinery / workarounds in place:
  - Flat-call A7 clamp (`recomp_push_return` + `g_recomp_initial_ssp`).
  - **NEW** top-level guest-stack trampoline (main.c) + `g_redirect_pending` propagation at JSR sites.
  - `g_halted` set by STOP; **nothing clears it yet** (no IRQ delivery — MC-CDI-007).
  - `g_irq_pending` recorded by `cdi_irq_raise`; nothing consumes it.
  - Two duplicated cycle models (interp `m68k_cycles` + recompiler `estimate_cycles_scc68070`) — TODO MC-CDI-009.

---

## Outstanding issues

1. **NEXT WALL (seq 276326): `$600000` bus error from the RECOMPILED tier.** `R16 @ $600000` at recompiled PC `$404B84` (a 68070 memory-sizing probe; D0=$00600000 at `$404B66`/`$404B74`, then $000500/$000506 = bus-error vector area). MC-CDI-004 made the **interpreter** raise a faithful vector-2 bus error via setjmp/longjmp around `exec_one`; the **recompiled tier can't unwind mid-instruction**, so `cdi_bus.c::bus_fault` → `m68k_interp_bus_error()` returns 0 (not armed) → fail-loud abort. CeDImu bus-errors every `$600000` read regardless of tier, so native must too.
2. **IRQ delivery (MC-CDI-007/010).** Shell idle is `STOP #$2000 @ $40A3E2` (confirmed by a prior 1M-step oracle run). `g_halted` parks there; nothing wakes it. Needs a timer/display IRQ vectored via `build_exception_frame` (short autovector frame) that clears `g_halted` and resumes.
3. **Unmodelled / fail-loud devices** still: CIAP ($300000–$303FFF), Timekeeper/NVRAM ($320000), MCD212 pixel pipeline + display-line IRQ, SCC68070 timer counting / DMA / MMU translation.
4. **Tooling debt:** store ring is native-only. If a memory-write divergence needs the oracle's writer value too, add a matching store capture to `oracle/cdi_oracle.cpp` (hook CeDImu bus writes).

---

## Next steps (priority order)

1. **Extend bus-error handling to the recompiled tier (the 276326 wall).** Mirror MC-CDI-004 at the recomp dispatch boundary:
   - Infra to reuse: `runner/src/m68k_interp.c` has `s_bus_env`/`s_bus_armed` + `m68k_interp_bus_error(addr)` (longjmps when armed). `runner/src/runtime.c::build_exception_frame(vec)` builds the faithful long (format-$F) frame and points `g_cpu.PC` at the handler without dispatching.
   - Plan: arm a `setjmp` at the recomp dispatch boundary (e.g. inside `recomp_call_addr` / the top-level trampoline, or a dedicated `recomp_run(pc)` wrapper). In `bus_fault`, when the interpreter unwind is NOT armed, build the vector-2 exception frame (`build_exception_frame(2)` — it already captures `g_last_access_addr` as TPF and `g_fault_opcode`), set `g_fault_addr`, then `longjmp` to that boundary. After the longjmp, dispatch `g_cpu.PC` (the OS-9 bus-error handler). The faulting recompiled instruction is abandoned (C has no cleanup; `g_cpu` + guest stack are the only state) — exactly how the interpreter handles it.
   - Validate: re-diff with `realign_divergence.py`; expect the wall to move well past 276326.
2. **Continue diffing past 276326** with the upgraded tools (a7top key + store ring) until the next true divergence.
3. **IRQ delivery (MC-CDI-007):** make STOP truly idle until an IRQ above the SR I-mask, vector via `build_exception_frame` (short frame), clear `g_halted`, resume from the trampoline. This is what lets the shell wake and the menu become live.
4. Consider committing the validated work (trampoline + 2 codegen fixes + tooling) before starting #1 — it's a clean milestone.

---

## Hard rules (always repeat)

- **No HLE stubs for OS-9.** Recompile the CD-RTOS system ROM and run it as native C. The clean-room hybrid interpreter (`runner/src/m68k_interp.c`, NOT clown68000 — AGPL) is the correctness floor for RAM-resident / dispatch-missed code. A subsystem is modeled faithfully or fails loud — never a plausible default.
- **No printf debugging / no log files.** Use the always-on trace ring + store ring + TCP debug server + `realign_divergence.py`. Query rings; never arm-then-run.
- **Never edit generated code** (`bios/generated/*.c`, `hotelmario/generated/*.c`). Fix the recompiler (`recompiler/src/`), the runner (`runner/src/`), or `game.cfg`. **Regen the BIOS after any `code_generator.c` change.**
- **CeDImu** is the behavioral oracle; **Ghidra** (68000 mode) is the literal oracle. State which you're using.
- **One runtime instance at a time:** `Get-Process CdiRuntime,CdiOracle -ErrorAction SilentlyContinue | Stop-Process -Force` before launching (and before rebuilding the runner — a held `--hold`/`--stop-seq` process locks `CdiRuntime.exe` against the linker).
- **Build with the PowerShell tool** (bash→powershell.exe hits a gcc `C:\Windows` temp-file bug). Set `$env:TMP`/`$env:TEMP` to `F:\Projects\cdirecomp\build\tmp` and use the mingw64 cmake `C:\msys64\mingw64\bin\cmake.exe`. The oracle (C++) needs `C:\msys64\mingw64\bin` on PATH at runtime for its DLLs.
- **AGPL/GPL boundary:** clown68000 (AGPL) and CeDImu's core (GPL) stay build-time/oracle-only. Nothing copyleft in the shipped runtime.
- **Port note:** a sibling project's `psx-beetle.exe` squats/auto-respawns on TCP 4380 (and was seen flaky-squatting 4390). Run the native on a clean port (this session used **4396**) and diff `--native 4396`; the oracle uses 4381.

---

## Build & run commands

```powershell
# (PowerShell tool) rebuild recompiler -> regen BIOS -> rebuild runner.
# Regen is REQUIRED after any code_generator.c edit; runner-only edits skip it.
Get-Process CdiRuntime,CdiOracle -ErrorAction SilentlyContinue | Stop-Process -Force
$env:TMP="F:\Projects\cdirecomp\build\tmp"; $env:TEMP="F:\Projects\cdirecomp\build\tmp"
$cm="C:\msys64\mingw64\bin\cmake.exe"
& $cm --build F:/Projects/cdirecomp/build/recompiler -j
& F:\Projects\cdirecomp\build\recompiler\CdiRecompBios.exe F:\Projects\cdirecomp\bios\cdi490a.rom --emit
& $cm --build F:/Projects/cdirecomp/build/runner -j
& $cm --build F:/Projects/cdirecomp/build/oracle  -j     # build once

# run + diff: oracle FIRST (fills its ring on 4381), then native on 4396 (4380/4390 squatted)
Get-Process CdiRuntime,CdiOracle -ErrorAction SilentlyContinue | Stop-Process -Force
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
Start-Process F:\Projects\cdirecomp\build\oracle\CdiOracle.exe  -ArgumentList "F:\Projects\cdirecomp\bios\cdi490a.rom","--steps","250000","--hold" -WindowStyle Hidden
Start-Process F:\Projects\cdirecomp\build\runner\CdiRuntime.exe -ArgumentList "F:\Projects\cdirecomp\bios\cdi490a.rom","--stop-seq","250000","--port","4396" -WindowStyle Hidden
python F:\Projects\cdirecomp\tools\realign_divergence.py --native 4396 --oracle 4381 --start 1 --window 8192 --context 3

# helpers (TCP debug surface):
#   tools/cdi_debug.py  --port {4396|4381} status | get_registers | trace --from S --count N | read_mem --addr A --len L
#   tools/trace_pcs.py  --port P --from S --count N
#   build/tmp/q.py P ADDR BEFORE COUNT           # query the 'stores' ring (last writers of an address)
#   CdiRuntime flags: --hold | --fault-hold | --stop-seq N | --port N    CdiOracle flags: --steps N | --hold
# To find the new endpoint, run native standalone (no --hold) and read the last [bus]/fault line:
#   & F:\Projects\cdirecomp\build\runner\CdiRuntime.exe F:\Projects\cdirecomp\bios\cdi490a.rom 2>&1 | Select-String "bus|fault|halt|returned"
```

`--stop-seq N` freezes the native rings-intact at seq N (deterministic stop for diffing; the boot no longer faults early so it would otherwise run past the window and evict it from the 262144 trace ring). Note: the trace ring holds 262144 entries, so for diffs from seq 1 keep N ≤ ~262144; for windows past that, use a later `--start` and stop both sides in the same window.

---

## Scope constraints

- **In scope (Phase C):** make the recompiled OS boot to the player shell, staying bit-exact to CeDImu. Editable: `recompiler/src/*`, `runner/src/*`, `runner/include/*`, the device/HLE device models, `oracle/cdi_oracle.cpp`, `tools/*`. Immediate task: recompiled-tier bus error (the 276326 wall), then continue the diff, then IRQ delivery.
- **Out of scope:** the game (Hotel Mario) — later phase, not until the shell boots and is interactive. Do NOT add OS-9 HLE stubs. Do NOT model `$600000` as a memory region (raising the bus error IS the faithful behavior). Do NOT link clown68000 / CeDImu's core into the shipped runtime. Do NOT edit generated `*.c`.
