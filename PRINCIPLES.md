# cdirecomp Principles

These extend the system-agnostic recomp principles (shared across
nesrecomp / snesrecomp / segagenesisrecomp / psxrecomp). They are not weaker
than those; CD-i just adds an OS layer.

## Ground Truth

- The original disc image and a trusted CD-i emulator (**CeDImu**, in
  `external/CeDImu`) are the behavioral source of truth. Generated C is
  evidence, not authority.
- For literal instruction/semantics questions, the disassembly of the OS-9
  module in question (via Ghidra, 68000 mode) is the literal oracle.
- Before debugging a symptom, confirm which build, generated tree, disc image,
  and game.cfg are actually in use.

## CD-i is an OS, not a cartridge

- A CD-i title runs on **CD-RTOS / OS-9**. Code arrives as relocatable OS-9
  modules loaded at run time; the program talks to the system through `TRAP #0`
  OS-9 calls (F$Load, F$Link, I$Read, …).
- We recompile the CD-RTOS system ROM itself (kernel + file managers + drivers +
  player shell) and run the real OS as native C. We do **NOT** hand-write OS-9
  HLE stubs — psxrecomp proved that path fails (faked syscall outputs + drifting
  C-side kernel state). The runtime emulates only the hardware chips (MCD212
  video, CIAP CD/audio, IKAT/SLAVE input, 68070 peripherals).
- Module load addresses and relocation are real program state. The flat address
  space the recompiled code runs in is defined by the loader, not assumed.

## No Stubs (especially here)

- The CD-i has a large OS + device surface, which makes silent stubs extra
  tempting and extra dangerous (the author's abandoned first PSX attempt failed
  exactly this way: stubs → silent failures → no progress).
- A subsystem is either modelled faithfully or it **fails loud** (abort with the
  exact address/PC/service). An unimplemented MCD212 register, OS-9 call, or
  memory region must stop the world with a precise message — never return a
  plausible default and continue.

## Hints Are Not Correctness

- game.cfg holds genuinely per-game facts (boot module, entry, module map,
  verified tables). It is not a place to silence a generator bug.
- If a cfg entry would paper over a decoder/codegen/runtime defect, fix the
  generator instead and let every CD-i title benefit.

## Control Flow Semantics

- Preserve 68000 semantics: JSR/BSR vs computed JMP, RTS/RTE, TRAP, tail calls,
  interrupt frames. The shared frontend already models these; CD-i adds the
  OS-9 `TRAP #0` gateway and CD-RTOS-driven re-entrancy/multitasking.
- OS-9 is multitasking (F$Fork/F$Chain). Concurrency is guest state; model it
  explicitly at the scheduler boundary, never by leaning on the host C stack.

## Observability: always-on ring buffers

- The debug surface is an **always-on ring buffer** queried after the fact,
  plus a TCP command server (see TCP.md / DEBUG.md). Never arm-a-trace-then-run,
  never pause/step two emulators to "sync" them — free-run and query the rings.
- If the data you need isn't captured, **extend the ring**, then query it. Do
  not fall back to printf or one-shot probes.

## Oracle parity

- The oracle is CeDImu embedded in-process, exposing the same ring/TCP surface
  as the native runtime, so native-vs-oracle diffs reduce to "what is the first
  divergence" at the frame / instruction / OS-call level.

## Validation

- A fix is done only when: the root cause is explained, the bug *class* is
  addressed (not one site), generated code is regenerated, both binaries build,
  and a deterministic oracle/smoke comparison exercises the behavior.
- Bootability is a milestone, not a finish line. The patterns we encode are
  game-agnostic and will be reused on the next CD-i title (Zelda CD-i discs sit
  next to Hotel Mario on the share).
