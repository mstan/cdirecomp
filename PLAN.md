# PLAN.md — cdirecomp maturation roadmap

Written 2026-06-17 after studying the sibling recompilers (`psxrecomp`,
`segagenesisrecomp`, `snesrecomp`, `nesrecomp`, `virtualboyrecomp`) and the
`recomp-template` scaffold. This is the *engineering* plan — where cdirecomp
sits in the family, what's missing, and the order to close the gaps. The
*feature* roadmap (device models, OS-9 loader, the game) lives in TODO.md as
`MC-CDI-*` tickets; this file sequences them and adds the cross-cutting
infrastructure the tickets assume.

## 1. The house style (what a mature sibling looks like)

Every sibling converges on the same shape:

```
<platform>recomp/
├── recompiler/   decoder → function-finder → codegen → emit  (ISA → C tool)
├── runner/       hardware sim + OS/HLE + SDL frontend, links the generated C
│   └── include/  the ONE contract header generated code compiles against
├── tools/        recompiler driver, debug client, dispatch-miss checker, lint
├── tests/        framework-level tests (decoder/codegen) + regression smoke
├── docs/         architecture, hardware notes, instruction-status, bring-up
├── external/     oracle emulator + ISA cycle oracle (submodules)
└── PRINCIPLES.md DEBUG.md CLAUDE.md TCP.md README.md TODO.md ENHANCEMENTS.md
```

The non-negotiable maturity markers, in the order siblings acquired them:

1. **Recompiler emits self-compiling C** for the target ROM (no stubs; unknown
   opcode → loud failure). cdirecomp: ✅ (60k lines of CD-RTOS C, builds).
2. **Runtime boots the recompiled code** and fails loud at the first unmodelled
   thing. cdirecomp: ✅ (boots through the SCC68070 exception model, then faults).
3. **Always-on ring buffers + a TCP debug server** that answers queries about
   the run *after the fact* — the single most-used debugging tool in every
   sibling, and the thing the global principles mandate. cdirecomp: ❌ stub.
4. **An independent oracle** (separately-authored emulator) exposing the *same*
   query surface, so native-vs-oracle first-divergence diffing is symmetric.
   cdirecomp: CeDImu cloned, not wired. ❌
5. **A regression smoke harness** (deterministic state snapshot at frame N,
   diffed vs a committed baseline). cdirecomp: ❌.
6. **Tripwires / coverage audits** (entry-state asserts, instruction-coverage
   reports). cdirecomp: partial (dispatch-miss monitor ✅; coverage doc ❌).

## 2. Honest maturity assessment

| Capability                         | psx | genesis | snes | nes | **cdi** |
|------------------------------------|-----|---------|------|-----|---------|
| ROM → self-compiling C             | ✅  | ✅      | ✅   | ✅  | ✅      |
| Runtime boots recompiled code      | ✅  | ✅      | ✅   | ✅  | ✅ (early) |
| Always-on ring + TCP debug server  | ✅  | ✅      | ✅   | ✅  | ❌ stub |
| Independent oracle, same surface   | ✅  | ✅      | ~   | ~   | ❌      |
| Regression smoke harness           | ✅  | ✅      | ✅   | ~   | ❌      |
| Device models do real work         | ✅  | ✅      | ✅   | ✅  | ❌ stubs |
| Boots to shell / plays a game      | ✅  | ✅      | ✅   | ✅  | ❌ (boot wall) |

cdirecomp is a **working scaffold**: the engine is proven on real CD-RTOS code,
the runtime executes it, and it fails loud exactly where the hardware model
runs out. It is blind, though — there is no way to see *how* a run reached a
fault. That blindness is the bottleneck, not any single device.

## 3. The 68000 frontend (provenance + reconciliation)

The frontend was copied verbatim from `segagenesisrecomp@5aa0c4f` on 2026-05-28
(see `recompiler/PROVENANCE.txt`). It has since **diverged** with CD-i-specific
fixes that must NOT be clobbered by a re-copy:

- `code_generator.c`  — ~256 lines changed (guest-PC-live, address-error, etc.)
- `function_finder.c` — ~149 lines changed
- `m68k_decoder.{c,h}` — ~17 lines (MOVEC handling)
- `m68k_validator`, `cycle_probe`, `annotations`, `codegen_diag` — identical.

**Decision:** do not re-copy. Treat the divergence as the seed of the shared
module. The standing task (TODO MC-CDI-009) is a proper 3-way reconciliation
that lands cdirecomp's fixes back upstream and parameterises the `genesis_*`
names, *then* extracts `m68k-recomp-core` as a submodule both projects consume.
A blind re-copy would regress committed work — the opposite of completeness.

## 4. Execution phases (this plan)

Ordered so each phase unblocks the next. Phase A is the lever: it makes every
later phase debuggable instead of blind.

### Phase A — Observability (the unblocker) — *this session*

The generated code already calls `glue_check_vblank` once per block with the
guest PC live (~9.5k call sites): a ready-made always-on trace tap that
currently does nothing. Turn it into the spine of the debug surface.

- **A1.** Always-on execution ring: capture {PC, opcode, D0-7/A0-7/SR} every
  block into a bounded evicting ring. Query backward; never arm-then-run.
- **A2.** Fault trail: on bus/address/illegal/movec abort, dump the last N ring
  entries (the path to the fault) — not just the final registers.
- **A3.** Real TCP server (winsock + POSIX), threaded so it answers while the
  run executes: `ping get_registers read_mem trace dispatch_miss_info
  bus_log quit`, JSON-line protocol per TCP.md.
- **A4.** `tools/cdi_debug.py` client mirroring the sibling debug clients.
- **A5.** `tools/check_dispatch_misses.py` — RULE 0a enforcement.

Exit criterion: from a second terminal, query the registers and the block trail
of a live/just-faulted boot; the fault dump shows the executed path into the
bus error at `$FFFFFFFC`.

### Phase B — Oracle parity (CeDImu, same surface) — *DONE 2026-06-17*

- ✅ Headless `oracle/cdi_oracle.cpp` links the wxWidgets-free CeDImu core,
  boots the same ROM (auto-detects Mono-IV), single-steps the SCC68070, and
  serves the SAME TCP trace surface on :4381.
- ✅ `tools/first_divergence.py`: free-run both, page the rings from seq 0, report
  the first PC mismatch. Proven: **36,896 instructions match the oracle exactly**,
  then diverge around `$400CB2–$400CB8` (seq ~36895) — a condition-code/branch
  bug, the next concrete debugging target.
- ⏳ Remaining: convert `external/CeDImu` to a submodule (MC-CDI-017); add
  emulator-internal probes (`emu_mcd212_state`, `framebuf_diff`) as devices land.

### Phase C — Boot to the player shell (the milestone) — *in progress*

First result (2026-06-17): with the oracle harness, the recompiled CD-RTOS is
**bit-exact to CeDImu for all 43,159 boot instructions** — zero codegen
divergence. Getting there hardened the harness: the trace tap moved to
instruction ENTRY (recompiler emits `debug_trace_block()` after each PC store)
so every instruction is sampled in order, JSR/BSR included — a hook at
instruction END misses the JSR sample and desyncs the streams. The only blocker
is the `$050A` RAM-built stub (dispatch miss) → the MC-CDI-011 hybrid
interpreter is the critical path, gated on the engine-licensing decision
(clown68000 AGPL / CeDImu GPL — both attach copyleft to the shipped runtime).

**MC-CDI-011 hybrid interpreter — DONE (2026-06-17), and AGPL-free.** Rather than
link clown68000 (AGPL), we ported segagenesisrecomp's own **clean-room** 68000
interpreter (`runner/src/m68k_interp.c`) — it reuses the shared decoder and
mirrors `code_generator.c`, so no copyleft attaches. Adapted the instruction
fetch to read from the CD-i bus (the missed code is RAM-resident, unlike the
Genesis ROM-only case). On a dispatch miss the runtime interprets the
un-recompiled gap until PC re-enters a recompiled function, then resumes native
(`m68k_interp_run_until_known` + `dispatch_has_addr`). Result: the boot bridges
the `$050A` exception stub and runs **2.3M+ instructions** of CD-RTOS init —
validated by the oracle (native lands exactly where CeDImu is during boot). So
the clean-room replacement the licensing plan deferred is already done; clown68000
stays a dev-only cycle-probe in the recompiler.

**Device bring-up via oracle-guided first-divergence (2026-06-17/18).** Each wall
is a status bit/region the runtime didn't model; each fix mirrors CeDImu exactly
and pushes the boot further:
- `$400E96` `BTST #7,$4FFFF1` — **MCD212 CSR1R DA** (Display Active). Modelled
  display-line timing (`mcd212_tick`, 262 lines/22 retrace NTSC, driven by CPU
  cycles from both tiers). Boot passes; frame counter now advances. ✓
- `$400B44` UART TX handshake — polls **USR bit 2 (TxRDY)** at `$80002013`.
  CeDImu sets TxRDY at reset and never clears it (instant transmit = always
  ready); added `periph_reset()` to mirror. Boot passes. ✓
- `$400790` — **unmapped bus read at `$600000`** during a memory-region probe
  (`D6=$FFFF8000` walking pattern). Next target: the Mono-IV `$600000` decode
  (MC-CDI-004) — model per CeDImu's bus so the probe resolves.

The pattern is steady: free-run → hit a loud wall → identify the exact register/
region in CeDImu → mirror it → advance. CeDImu reaches a shell-idle loop at
`$40A3E2` (user mode), the boot-to-shell finish line.

Then drive the remaining MC-CDI device tickets, oracle-diffed, in first-divergence order:
MMU/memory (MC-CDI-006) → MCD212 register file already present, add decoders
(MC-CDI-012) → IKAT (MC-CDI-023) → Timekeeper (MC-CDI-022) → interrupt delivery
+ pacing (MC-CDI-007/010) → hybrid interpreter for RAM-built stubs (MC-CDI-011).
Each lands with an oracle diff showing the divergence closed.

### Phase D — Regression + coverage discipline

- `tools/boot_smoke.py`: snapshot system RAM + regs at a fixed OS-9-call count,
  diff vs committed baseline (baseline updates = same commit as the change).
- `COVERAGE.md`: 68000 instruction-coverage audit for the SCC68070 (port the
  Genesis COVERAGE.md method; note SCC68070-only opcodes).

### Phase E — The game (Hotel Mario)

Phase 3 in TODO.md: OS-9 module-loader bridge (MC-CDI-024), recompile
`cdi_hotel` + streamed level modules (MC-CDI-025), CIAP CD/audio (MC-CDI-013),
gameplay oracle (MAME `cdimono`, MC-CDI-026).

## 5. Scaffolding gaps to close alongside

- `ENHANCEMENTS.md` — the one canonical doc missing (added with this plan).
- `tools/ tests/ docs/` — currently empty dirs; Phase A seeds tools/ and docs/.
- `external/CeDImu` → submodule (Phase B).

## 6. What "done" looks like for each phase

A phase is done when: the code is regenerated/rebuilt clean, the new capability
is exercised end-to-end (not just compiled), the result is stated with concrete
numbers, and — from Phase B on — an oracle diff confirms the divergence it
targeted is closed. No "quick X now, proper Y later." (Global rule.)
