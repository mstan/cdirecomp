# ARCHITECTURE.md — how cdirecomp fits together

Two binaries plus an oracle, the same two-tier shape as every sibling recomp.

```
   CD-RTOS ROM ──► CdiRecompBios ──► bios/generated/cdrtos_{full,dispatch}.c
   (user-provided)  (recompiler)         (recompiled OS-9 kernel as C)
                                               │
                                               ▼
   runner/src/* (hand-written CD-i machine) + generated C ──► CdiRuntime
                                               │
                                               ▼
                          boots the recompiled OS, fails loud at the
                          first unmodelled device  ◄──diff── CeDImu (oracle)
```

## Recompiler (`recompiler/`, builds `CdiRecomp` / `CdiRecompBios`)

The 68000 frontend, copied from segagenesisrecomp (PROVENANCE.txt) and since
extended for CD-i. Pipeline: `rom_parser`/`disc_parser` → `function_finder`
(reachability from reset + seeds) → `m68k_decoder` → `m68k_validator` →
`code_generator` (per-instruction C, cycle-probed via clown68000). Output is
self-compiling C: every function becomes `void func_XXXXXX(void)`, plus a
dispatch table mapping guest address → function. Unknown opcode → loud failure,
never a stub. `main_cdi_bios.c` drives the flat-ROM (BIOS) path;
`main_cdi.c` the disc/module path.

## Runner (`runner/`, builds `CdiRuntime`)

The hand-written CD-i machine the generated C links against. Contract:
`runner/include/cdi_runtime.h` (the generated↔runner ABI) and
`runner/include/debug_server.h` (the runtime-internal debug surface).

- `cdi_bus.c` — the memory model: RAM banks, ROM window, MMIO routing. The ONE
  place generated code reaches memory (m68k_read/write*). Unmapped → loud abort.
- `runtime.c` — CPU-state ABI, the SCC68070 exception model (faithful CeDImu
  port: frame push + RAM vector-table dispatch), dispatch-miss accounting,
  flat-call trampolines, the per-block hook.
- `mcd212.c cdic.c slave.c periph.c` — device models (register files present;
  decoders/behavior are the Phase-C tickets). Each fails loud on an unmodelled
  register rather than faking a value.
- `cdrtos.c` — the `TRAP #0` OS-9 gateway (vestigial; the goal is to route every
  vector through the recompiled kernel, not HLE it).
- `debug_server.c` — always-on ring buffers + threaded TCP server (below).
- `main.c` — loads the ROM, seeds SCC68070 reset state, drives execution from
  the reset entry, holds open for inspection with `--hold`.

## Observability (the spine — read DEBUG.md, TCP.md)

Always-on, queried after the fact; never arm-then-run.

- **Execution-trace ring** (262144 entries): every instruction's PC + full
  register file, captured from a hook the recompiler emits at instruction ENTRY
  (`debug_trace_block()`, right after the guest-PC store). Entry sampling is
  deliberate: one in-order sample per instruction, JSR/BSR included (a hook at
  instruction END dives into the callee before sampling, desyncing the stream
  from the oracle). This is the trail that turns "aborted at $X" into "here are
  the instructions that led there" — and the basis of first-divergence diffing.
- **Frame ring** (36000 entries): per-frame snapshot (grows with the runtime).
- **Fault trail**: every abort site (`bus_fault`, illegal opcode, movec) dumps
  the ring tail to stderr before dying — the executed path into the crash.
- **TCP server** (127.0.0.1:4380; oracle +1): threaded, answers while the run
  executes. `ping status get_registers read_mem trace dispatch_miss_info quit`.
  Client: `tools/cdi_debug.py`. RULE 0a gate: `tools/check_dispatch_misses.py`.

## Oracle (`oracle/` + `external/CeDImu`)

Independent C++ CD-i emulator — behavioral ground truth. `oracle/cdi_oracle.cpp`
links CeDImu's wxWidgets-free core library (`external/CeDImu/src/CDI` builds a
standalone `CeDImu` static lib; only the GUI needs wx), boots the same ROM
(`CDI::NewCDI(Boards::AutoDetect, …)` → Mono-IV), single-steps the SCC68070
(`m_cpu.Run(false)`) capturing per-instruction `{currentPC, registers}`, and
serves the SAME TCP surface as the native runtime on :4381. So
`tools/first_divergence.py` pages both rings from seq 0 and reports the first PC
mismatch — DEBUG.md's first-divergence loop, automated. For literal instruction
semantics the oracle is Ghidra over the OS-9 module (68000 mode).

**Build quirk:** the oracle is C++23 and must be configured with the **mingw64
cmake** (`C:/msys64/mingw64/bin/cmake.exe`), not the devkitPro cmake that may be
first on PATH (it mangles absolute Windows compiler paths). In the sandbox, also
set `TMP`/`TEMP` to a writable dir (e.g. `build/tmp`) — gcc's default temp is
`C:\Windows`. Both are one-time environment quirks, not project config.

## Build order (OS-first)

1. `CdiRecompBios bios/<rom> --emit` → `bios/generated/*.c`
2. build `CdiRuntime` (links the generated OS) → boot, diff vs CeDImu
3. only once the shell boots: recompile the game (`cdi_hotel`, level modules)
