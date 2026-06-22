# Session Handoff — cdirecomp (Philips CD-i SCC68070 → C static recompiler)

**Date:** 2026-06-21
**Phase:** Phase C — boot the recompiled CD-RTOS to the player shell (PLAN.md). Phases A (observability) and B (CeDImu oracle parity) are done.

> Read this handoff + the `cdi-boot-status` auto-memory + `CLAUDE.md`/`PRINCIPLES.md`/`DEBUG.md`. This handoff is authoritative for the most recent work. **Everything described below is COMMITTED and pushed** (tree is clean, `master` == `origin/master` == `67e737a`).

---

## Headline

The `$600000` memory-sizing-probe wall (the read **from the recompiled tier** at seq **276326** that the old bus path couldn't turn into a faithful vector-2 bus error) is **beaten**. The boot now runs **oracle-validated through seq 400000** with **no true divergence** (realign over both `[1,250000)` and `[138000,400000)`: 773 + 552 benign re-syncs). The run no longer hits a wall — it stops because `--stop-seq` told it to.

All three fixes this session were **runner-side only** — no `code_generator.c` change, so **no BIOS regen** was needed.

The repo is now a **private GitHub repository: `github.com/mstan/cdirecomp`** (SSH, master). ROMs / `build/` / generated C / the GPL `external/CeDImu` clone are gitignored and confirmed absent from the remote; vendored `external/clown68000` source is included.

---

## What was done this session

Two commits, both pushed:
- `5672269` — **boot: trampoline + codegen + tooling** (the prior session's validated milestone, advance to seq 276326 / MC-CDI-012).
- `67e737a` — **runtime: faithful recompiled-tier `$600000` bus error** (this session's work, boot faithful to seq 400000 / MC-CDI-004).

### 1. Recompiled-tier bus error (the 276326 wall) — MC-CDI-004 extended
The interpreter already raised a faithful vector-2 bus error by arming `setjmp(s_bus_env)` around `exec_one`. The **recompiled tier reaches memory through plain C we cannot unwind mid-instruction**, so it needed a landing pad one level up:
- `runner/src/main.c` (top-level trampoline) arms a landing pad: `g_recomp_bus_armed = 1; if (setjmp(g_recomp_bus_env) != 0) { re-arm; g_redirect_addr = g_cpu.PC; g_redirect_pending = 1; }`.
- `runner/src/cdi_bus.c` (`bus_fault`): after the interpreter path returns 0 (not armed), calls `recomp_bus_error(addr)` before the fail-loud path.
- `runner/src/runtime.c` `recomp_bus_error(addr)`: if armed, decode the faulting instruction via `m68k_interp_decode_at()` (reuses `busview`, **preserves `g_last_access_addr`** so the TPF survives), set `g_fault_opcode` + `g_cpu.PC = post-fetch PC`, restore the TPF, `build_exception_frame(2)`, `mcd212_tick(158)`, `longjmp(g_recomp_bus_env, 1)`.
- The landing pad re-dispatches `g_cpu.PC` = the OS-9 bus-error handler (`$000500`). The handler is **RAM-resident** (the recompiler never saw it) so it runs entirely in the hybrid interpreter — handler + RTE + resume to `$404B86` — in one shot, no `g_rte_pending` plumbing needed there.

### 2. Two flat-call/hybrid interop bugs (exposed by getting past the bus error)
- **Depth-0 `g_rte_pending` hygiene** (`main.c`): the trampoline now clears `g_rte_pending` before **every** depth-0 dispatch. A skip-RTS that bubbles to the top leaves `g_cpu.PC` = the RTS's own stale address (the real target is `[A7]`); a fresh entry inheriting a propagating skip would make its first JSR site wrongly bail. (Fixes the native-293763 `$404714` duplicate → `$500` divergence.)
- **`recomp_top_resume()` for the `pea/pea/rts` idiom** (`runner/src/runtime.c` + decl in `cdi_runtime.h`): the trampoline's `[A7]`-follow pre-pops assuming flat-call (recompiled RTS is a pure C `return`). A **non-entry** target routes to the hybrid interpreter, whose RTS self-pops the next return; the old ret-stop then stopped there and the loop double-followed `[A7]` (`$40839C` popped +8 → `$480000` vs oracle +4 → `$406A9C`). At depth 0 there is no C caller to return to, so a non-entry target is now interpreted with **no ret-stop** (`m68k_interp_run_until_known(target, 0)`) — flowing through its own returns until it re-enters a recompiled entry. Entry targets still flat-call via `recomp_call_addr`.

### 3. Supporting infra added this session
- `runner/include/m68k_interp.h` + `runner/src/m68k_interp.c`: `m68k_interp_decode_at(pc, out)` — decode at `pc`, side-effect-free w.r.t. `g_last_access_addr`; returns `byte_length` or 0.
- `runner/include/cdi_runtime.h`: `#include <setjmp.h>`; `extern jmp_buf g_recomp_bus_env; extern int g_recomp_bus_armed; int recomp_bus_error(uint32_t);`; `void recomp_top_resume(uint32_t);`.
- `.gitignore`: added `__pycache__/` + `*.pyc`.

### Files modified this session
```
runner/include/cdi_runtime.h    (setjmp.h, g_recomp_bus_env/armed, recomp_bus_error, recomp_top_resume)
runner/include/m68k_interp.h     (m68k_interp_decode_at decl)
runner/src/m68k_interp.c         (m68k_interp_decode_at)
runner/src/runtime.c             (recomp_bus_error, recomp_top_resume, g_recomp_bus_env/armed defs)
runner/src/cdi_bus.c             (bus_fault -> recomp_bus_error fallback)
runner/src/main.c                (recomp-tier setjmp landing pad; depth-0 g_rte_pending clear; recomp_top_resume)
.gitignore                       (__pycache__, *.pyc)
```
**No `code_generator.c` change → BIOS was NOT regenerated this session.** (348 recompiled functions, unchanged.)

---

## Architecture review + interpreter-fallback classifier (this session, UNCOMMITTED)

Conferred with the ChatGPT "recomp" thread (architecture review; see the
`architecture-dispatcher-direction` and `chatgpt-architecture-consult` memories).
**Verdict:** flat-call is the wrong *semantic* base for OS-9 — the guest stack is
a scheduler data structure, not advisory metadata. Target model: a guest-PC/
guest-stack-driven central dispatcher (RTS pops `[A7]`; blocks return an
`ExitResult`); flat-call C calls become an *optimization*; interpreter is the
universal fallback; exceptions/IRQs are dispatcher exits (longjmp only as a local
escape hatch). 5-stage migration; the trampoline + `g_redirect_pending`
uncleared-propagation is "a central dispatcher reinvented through the side door."

**User chose the data-driven first step: classify the interpreter fallback before
deciding sequencing.** Built an always-on classifier (runner + tool only; no
`code_generator.c`, no BIOS regen):
- `runner/include/debug_server.h`: `FB_*` reason enum, `extern int g_fallback_reason`, `debug_trace_interp()` decl.
- `runner/src/debug_server.c`: per-PC open-addressing aggregate (`s_fb_hash`, 64K slots) tagged by reason + region (`fb_region_of`); `interp_report` TCP command (`count`/`reason`/`reset` params); `status` now also reports `interp` (total interpreted insns).
- `runner/src/m68k_interp.c`: `debug_trace_interp(pc)` hook in `m68k_interp_step` (next to the trace-ring sample) + `#include "debug_server.h"`.
- `runner/src/runtime.c`: sets `g_fallback_reason` at the three entry points — `hybrid_enter`=`FB_DISPATCH_MISS`, `recomp_top_resume`=`FB_TOP_RESUME` (or a one-shot pending), `recomp_bus_error` sets a one-shot `FB_BUS_HANDLER` consumed by the next `recomp_top_resume`.
- `tools/interp_report.py`: formats the report (hex PCs, %s, reason/region names).

**Measured result (boot → seq 400000):** interpreter runs **40.3 %** of executed
instructions (161,209 interp vs 238,754 native), and that fallback is **97.9 %
`dispatch_miss`, 99 % in ROM** — tight clusters of statically-present ROM
functions (~`$400B00–$401300`, `$400E00–$400F00`) the recompiler never discovered,
reached by indirect dispatch (`call_by_address`) that static discovery didn't
follow. By ChatGPT's own rule of thumb this is a **recompiler-coverage smell, not
expected OS behavior**. Crucially the existing dispatch-miss monitor (RULE 0a)
reported **`miss_count=0`** throughout, because `hybrid_enter` resolves these
misses *successfully* without logging — the coverage loss was invisible until this
tool. `bus_handler` (0.4 %, the `$500` OS-9 handler) and `top_resume` (1.7 %,
`pea/pea/rts`) are small and expected.

**Implication:** biggest near-term lever = improve the recompiler's static
discovery to follow indirect-dispatch targets in ROM and recompile those
functions (complements — does not replace — the dispatcher migration). Sequencing
(this coverage win vs. Stage-1 dispatcher migration vs. IRQ delivery) is awaiting
a user decision.

---

## Verified state

- **Builds clean** (mingw64 cmake): `CdiRecomp`, `CdiRecompBios`, `CdiRuntime`, `CdiOracle`.
- **Faithful boot through seq 400000**, no true divergence. Realign was run over two overlapping windows because the native trace ring (262144 entries) can't hold `[1,400000)` at once: `[1,250000)` (773 benign re-syncs) and `[138000,400000)` (552 benign re-syncs).
- The `$600000` read at recompiled PC `$404B84` now raises a faithful vector-2 bus error; the OS-9 handler at `$000500` runs in the interpreter and resumes to `$404B86`.
- `dispatch_miss` count = 0 on this path.
- **Repo pushed & verified clean/private:** `github.com/mstan/cdirecomp`, 96 tracked files, no ROMs / `build/` / generated C / CeDImu clone on the remote; local in sync with `origin/master`.

---

## Current runtime state

- ROM: `bios/cdi490a.rom` (512 KB, Mono-IV / 32 KB NVRAM / NTSC). Reset SSP=$1500, PC=$4004B8, SR=$2700. **Not in the repo** (copyrighted; gitignored). A fresh clone must supply it locally and run `CdiRecompBios … --emit` to regenerate `bios/generated/*.c` before building the runner.
- Static dispatch table: **348** recompiled functions.
- Active machinery / workarounds in place:
  - Flat-call A7 clamp (`recomp_push_return` + `g_recomp_initial_ssp`).
  - Top-level trampoline (main.c): follows `[A7]` / `g_redirect_pending` when a recompiled RTS bottoms out to main; **recomp-tier bus-error setjmp landing pad**; clears `g_rte_pending` before every depth-0 dispatch; routes non-entry targets through the interpreter with no ret-stop (`recomp_top_resume`).
  - JSR-site `g_redirect_pending` propagation (code_generator.c) for context switches.
  - Hybrid interpreter (`m68k_interp.c`, clean-room) runs RAM-resident / dispatch-missed code; `m68k_interp_run_until_known(entry, stop_pc)` interprets until re-entering a recompiled dispatch ENTRY (`M68KI_REENTER`) or `pc == stop_pc`.
  - `g_halted` set by STOP; **nothing clears it yet** (no IRQ delivery — MC-CDI-007).
  - `g_irq_pending` recorded by `cdi_irq_raise`; nothing consumes it.
  - Two duplicated cycle models (interp `m68k_cycles` + recompiler `estimate_cycles_scc68070`) — TODO MC-CDI-009.

---

## Outstanding issues

1. **Run past seq 400000 to the shell-idle STOP.** The run stops at `--stop-seq`, not a wall — no known divergence ahead. Shell idle is `STOP #$2000 @ $40A3E2` (confirmed by a prior 1M-step oracle run). Keep diffing forward (later `--start`, both sides stopped in the same window) to confirm faithfulness all the way to that STOP.
2. **IRQ delivery (MC-CDI-007/010).** `g_halted` parks at the shell-idle STOP; nothing wakes it. Needs a timer/display IRQ above the SR I-mask, vectored via `build_exception_frame` (short autovector frame), that clears `g_halted` and resumes from the trampoline. This is what makes the menu live.
3. **Unmodelled / fail-loud devices** still: CIAP ($300000–$303FFF), Timekeeper/NVRAM ($320000), MCD212 pixel pipeline + display-line IRQ, SCC68070 timer counting / DMA / MMU translation.
4. **Interpreter runs 40 % of boot — and it's a COVERAGE smell, now measured.** The classifier shows 97.9 % `dispatch_miss`, 99 % ROM: statically-present ROM functions the recompiler missed (reached by indirect dispatch). NOT relocated/generated RAM. Fix = improve static discovery to follow `call_by_address` targets in ROM so they get recompiled. (The old `miss_count` monitor is blind to these — they're "handled" by the hybrid.)
5. **Tooling debt:** store ring is native-only. If a memory-write divergence needs the oracle's writer value too, add a matching store capture to `oracle/cdi_oracle.cpp` (hook CeDImu bus writes). Realign must diff in windows when the span exceeds ~262144 (trace-ring capacity).

---

## Next steps (priority order)

Sequencing is **awaiting a user decision** among: (A) the ROM-coverage win below,
(B) Stage-1 of the dispatcher migration (replace `g_redirect_pending` uncleared-
propagation with structured `ExitResult` returns), (C) IRQ delivery on the current
model. The classifier now gives the data to choose; the architecture review favors
(B) before building IRQ on another longjmp/trampoline escape.

1. **ROM coverage (the measured 40 % fallback lever).** Improve recompiler static
   discovery to follow indirect-dispatch (`call_by_address`) targets in ROM so the
   missed functions (~`$400B00–$401300`, `$400E00–$400F00`) get recompiled. Re-run
   `tools/interp_report.py` to confirm the dispatch_miss bucket shrinks. (NOTE: a
   `code_generator.c`/discovery change here DOES require a BIOS regen.)
2. **Stage-1 dispatcher migration** (per the architecture review): structured
   `ExitResult` returns replacing `g_redirect_pending` propagation, keeping
   flat-call working — so IRQ delivery lands on a dispatcher, not a new escape.
3. **IRQ delivery (MC-CDI-007):** STOP idle until an IRQ above the SR I-mask;
   vector via `build_exception_frame`, clear `g_halted`, resume. Wakes the shell.
4. **Continue diffing past 400000 toward `STOP #$2000 @ $40A3E2`** (a7top key +
   store ring + realign), confirming no true divergence to the shell-idle STOP.

New tooling this session: `tools/interp_report.py [--port N] [--count K] [--reason R] [--reset]`
and the `interp_report` TCP command — the always-on fallback classifier.

---

## Hard rules (always repeat)

- **No HLE stubs for OS-9.** Recompile the CD-RTOS system ROM and run it as native C. The clean-room hybrid interpreter (`runner/src/m68k_interp.c`, NOT clown68000 — AGPL) is the correctness floor for RAM-resident / dispatch-missed code. A subsystem is modeled faithfully or fails loud — never a plausible default.
- **No printf debugging / no log files.** Use the always-on trace ring + store ring + TCP debug server + `realign_divergence.py`. Query rings; never arm-then-run, never pause/step to synchronize observers.
- **Never edit generated code** (`bios/generated/*.c`, `hotelmario/generated/*.c`). Fix the recompiler (`recompiler/src/`), the runner (`runner/src/`), or `game.cfg`. **Regen the BIOS after any `code_generator.c` change** (none needed this session — fixes were runner-only).
- **CeDImu** is the behavioral oracle; **Ghidra** (68000 mode) is the literal oracle. State which you're using.
- **One runtime instance at a time:** `Get-Process CdiRuntime,CdiOracle -ErrorAction SilentlyContinue | Stop-Process -Force` before launching (and before rebuilding the runner — a held `--hold`/`--stop-seq` process locks `CdiRuntime.exe` against the linker).
- **Build with the PowerShell tool** (bash→powershell.exe hits a gcc `C:\Windows` temp-file bug). Set `$env:TMP`/`$env:TEMP` to `F:\Projects\cdirecomp\build\tmp` and use the mingw64 cmake `C:\msys64\mingw64\bin\cmake.exe`. The oracle (C++) needs `C:\msys64\mingw64\bin` on PATH at runtime for its DLLs.
- **AGPL/GPL boundary:** clown68000 (AGPL) and CeDImu's core (GPL) stay build-time/oracle-only. Nothing copyleft in the shipped runtime. **CeDImu's working tree must NOT be committed** (gitignored).
- **ROMs are not redistributable** — `bios/*.rom` and any disc image stay gitignored.
- **Port note:** a sibling project's `psx-beetle.exe` squats/auto-respawns on TCP 4380 (and was seen flaky-squatting 4390). Run the native on a clean port (recent sessions used **4396**) and diff `--native 4396`; the oracle uses 4381.

---

## Build & run commands

```powershell
# (PowerShell tool) rebuild recompiler -> regen BIOS -> rebuild runner.
# Regen is REQUIRED after any code_generator.c edit; runner-only edits skip it (this session was runner-only).
Get-Process CdiRuntime,CdiOracle -ErrorAction SilentlyContinue | Stop-Process -Force
$env:TMP="F:\Projects\cdirecomp\build\tmp"; $env:TEMP="F:\Projects\cdirecomp\build\tmp"
$cm="C:\msys64\mingw64\bin\cmake.exe"
& $cm --build F:/Projects/cdirecomp/build/recompiler -j
& F:\Projects\cdirecomp\build\recompiler\CdiRecompBios.exe F:\Projects\cdirecomp\bios\cdi490a.rom --emit   # only after a code_generator.c change
& $cm --build F:/Projects/cdirecomp/build/runner -j
& $cm --build F:/Projects/cdirecomp/build/oracle  -j     # build once

# run + diff: oracle FIRST (fills its ring on 4381), then native on 4396 (4380/4390 squatted)
Get-Process CdiRuntime,CdiOracle -ErrorAction SilentlyContinue | Stop-Process -Force
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
Start-Process F:\Projects\cdirecomp\build\oracle\CdiOracle.exe  -ArgumentList "F:\Projects\cdirecomp\bios\cdi490a.rom","--steps","400000","--hold" -WindowStyle Hidden
Start-Process F:\Projects\cdirecomp\build\runner\CdiRuntime.exe -ArgumentList "F:\Projects\cdirecomp\bios\cdi490a.rom","--stop-seq","400000","--port","4396" -WindowStyle Hidden
# diff in windows (trace ring = 262144 entries; can't span [1,400000) at once):
python F:\Projects\cdirecomp\tools\realign_divergence.py --native 4396 --oracle 4381 --start 138000 --window 8192 --context 3

# helpers (TCP debug surface):
#   tools/cdi_debug.py  --port {4396|4381} status | get_registers | trace --from S --count N | read_mem --addr A --len L
#   tools/trace_pcs.py  --port P --from S --count N
#   build/tmp/q.py P ADDR BEFORE COUNT           # query the 'stores' ring (last writers of an address)
#   CdiRuntime flags: --hold | --fault-hold | --stop-seq N | --port N    CdiOracle flags: --steps N | --hold
# To find an endpoint, run native standalone (no --hold) and read the last [bus]/fault/halt line:
#   & F:\Projects\cdirecomp\build\runner\CdiRuntime.exe F:\Projects\cdirecomp\bios\cdi490a.rom 2>&1 | Select-String "bus|fault|halt|returned"
```

`--stop-seq N` freezes the native rings-intact at seq N (deterministic stop for diffing; the boot no longer faults early so it would otherwise run past the window and evict it from the 262144 trace ring). For windows past ~262144, use a later `--start` and stop both sides in the same window.

---

## Scope constraints

- **In scope (Phase C):** make the recompiled OS boot to the player shell, staying bit-exact to CeDImu. Editable: `recompiler/src/*`, `runner/src/*`, `runner/include/*`, the device/HLE device models, `oracle/cdi_oracle.cpp`, `tools/*`, `hotelmario/game.cfg`. Immediate task: continue the diff past 400000 to the shell-idle STOP, then IRQ delivery.
- **Out of scope:** the game (Hotel Mario) — later phase, not until the shell boots and is interactive. Do NOT add OS-9 HLE stubs. Do NOT model `$600000` as a memory region (raising the bus error IS the faithful behavior). Do NOT link clown68000 / CeDImu's core into the shipped runtime. Do NOT edit generated `*.c`. Do NOT commit ROMs or CeDImu's working tree.
