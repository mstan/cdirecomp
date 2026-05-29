# DEBUG LOOP (execution contract)

All inspection goes through the TCP debug server and the always-on ring
buffers (see TCP.md). No printf debugging; no arm-trace-then-run; no
pause/step to "sync" native and oracle. Free-run, then query the rings.

## RULE 0 — Tool validation (first use)

On first use of any tool (Ghidra, the disc parser, the decoder, a TCP command),
cross-check its output against another source and confirm structure + content.
Unvalidated tool output invalidates all reasoning built on it.

## RULE 0a — Dispatch-miss check (every run, before anything else)

`call_by_address` logs a dispatch miss when no generated function exists at a
target. A miss means an entire subroutine was skipped — a silent game-breaking
bug. Resolve all misses (discover the function, fix the finder/loader) before
debugging anything else.

## THE LOOP

1. **Sync** — define what "the same point in execution" means across native and
   oracle. Not wall-frame number alone; use a hardware/OS event (VSYNC count,
   OS-9 call count, sector-read count).
2. **Dump state** from the ring buffer on both sides.
3. **Diff** — find the bytes/registers that differ.
4. **First divergence** — walk the ring backwards to the FIRST frame/block where
   they diverge. Later differences are consequences.
5. **Trace the writer / executed edge** — for a state bug, find the instruction
   that wrote the wrong value; for a control-flow bug, the edge that was taken.
6. **Classify** — discovery/codegen, runtime/timing, memory/bus, OS-9 HLE,
   device (MCD212/CDIC/SLAVE), or game metadata.
7. **Fix the generator/runtime** — never the generated C, never a one-off cfg
   hint. Fix the class.
8. **Regenerate → build → run → measure → commit** with concrete numbers.

If any step is skipped → STOP and restart.

## CD-i first-divergence ladder

Because CD-i boots through CD-RTOS, expect the earliest divergences here, in
order:

1. **OS-9 loader / `TRAP #0` HLE** — wrong relocation, wrong service result,
   wrong errno in D1. Trace the exact OS-9 call (cdrtos.c logs name + regs).
2. **Memory model / MMU** — module loaded at the wrong base, or an MMU mapping
   not modelled. The bus aborts with the exact address.
3. **Device programming** — MCD212 register the game writes before video comes
   up; CDIC sector request; SLAVE input poll. Each device aborts loud with the
   register touched.
4. **Timing / interrupts** — display-line IRQ pacing, CDIC data-ready IRQ.

## When documenting a finding, state at minimum

target behavior · oracle used (CeDImu TCP / Ghidra / disc) · sync point · diff
(subsystem, address, expected, actual) · first divergence (frame/block index,
not an eyeball) · writer (module + PC + call path) · classification · minimal
generator/runtime fix · re-test plan. Missing a section → STOP.
