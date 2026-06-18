# RESUME.md — pick up here next session

Single source for resuming the CD-i boot bring-up. Pairs with PLAN.md (the
maturation roadmap) and the `cdi-boot-status` auto-memory. Last updated end of
the 2026-06-18 session.

## Where the boot is

The recompiled CD-RTOS boots **bit-exact to the CeDImu oracle for ~66,800
instructions** (full register file, verified by `tools/first_divergence.py`),
deep into kernel init. Cleared walls this far:

1. `$050A` RAM exception stub → clean-room hybrid interpreter (MC-CDI-011).
2. `$400E96` `BTST #7,$4FFFF1` (MCD212 DA) → display-line timing model.
3. `$400B44` UART TX → USR TxRDY set at reset.
4. `$40069E` deliberate OS-9 address error → faithful exception frame
   (PC/TPF = odd target, IRC/IR = faulting opcode). This unblocked the OS-9
   exception handler and ~23.7k more matched instructions.

## THE NEXT TASK — MC-CDI-005, cycle-accurate interpreter timing

**Current first divergence: seq 66821, PC `$400E8C`.** A vertical-sync loop:

```
$400E8C: BTST #7,$4FFFF1   ; test MCD212 CSR1R DA (Display Active)
$400E94: BNE  $400E8C      ; wait for DA = 0  (enter retrace)
$400E96: BTST #7,$4FFFF1
$400E9E: BEQ  $400E96      ; then wait for DA = 1 (active)  -> frame sync
```

At the divergence **every register matches**; only the **DA phase** differs
(native reads DA=1, oracle DA=0). Cause: the MCD212 DA bit is driven by
accumulated CPU cycles (`mcd212_tick`), but the **interpreter ticks a flat ~10
cycles/instruction** (`runner/src/m68k_interp.c`, `m68k_interp_step` →
`mcd212_tick(10)`) while CeDImu uses **exact per-instruction SCC68070 cycles**.
So DA toggles a beat off and the sync loop lands on a different iteration.

This is a timing-accuracy issue, not a correctness bug. The downstream
`$600000` fault (and everything past seq 66821) is a consequence — fixing the
timing should carry the boot toward CeDImu's shell-idle loop at `$40A3E2`.

### How to fix (give the interpreter real cycle counts)

The recompiled tier is already accurate: the generator embeds per-instruction
cycle costs (`g_cycle_accumulator += N`, from `estimate_cycles`/`cycle_probe`
which uses clown68000 at BUILD time only). The interpreter needs the same.

Options, cleanest first (keep the runtime AGPL-free — do NOT link clown68000
into the runner):

- **A. Clean-room SCC68070 cycle table in the runner.** Add a function
  `m68k_cycles(const M68KInstr*)` (mirror `estimate_cycles` in
  `recompiler/src/code_generator.c`, which already encodes the costs by
  mnemonic/EA/size) and call `mcd212_tick(m68k_cycles(&ins))` instead of
  `mcd212_tick(10)`. Validate the table against the oracle (below). This is the
  faithful MC-CDI-005 and reusable for frame pacing.
- **B. Emit an address→cycles table.** The recompiler already probes every
  recompiled instruction; emit a `{addr: cycles}` table into the generated
  output and have the interpreter look up by PC when in ROM. Simpler but only
  covers recompiled addresses (RAM-built code still needs a fallback) — the
  frame-sync loop IS in ROM, so this would unblock it.
- The flat `mcd212_tick(10)` is the stand-in to replace.

Also relevant: `MCD_CYCLES_LINE 986` in `runner/src/mcd212.c` is an estimate
(SCC68070 15.5 MHz / 262 lines / 60 Hz). CeDImu uses ns-based line timing
(`GetLineDisplayTime` = 64000 ns) — reconcile if the phase is still off after
the per-instruction fix.

### Verify success

After the fix, re-run the divergence (workflow below). Success = the divergence
moves well past seq 66821 (ideally the boot reaches `$40A3E2`, the oracle's
shell-idle loop, or a brand-new device wall). The DA-phase mismatch at
`$400E8C` should be gone.

## Validation workflow (READ — non-obvious environment gotchas)

- **Build with the PowerShell tool**, not bash→`powershell.exe`. The latter
  hits a gcc "Cannot create temporary file in C:\Windows" bug. Always set
  `$env:TMP`/`$env:TEMP` to `F:\Projects\cdirecomp\build\tmp` first, and use the
  **mingw64 cmake** `C:\msys64\mingw64\bin\cmake.exe` (the devkitPro cmake first
  on PATH mangles Windows paths).
- **The oracle is C++**; launching it needs `C:\msys64\mingw64\bin` on PATH for
  its mingw runtime DLLs, or it silently fails to start (native C build is fine).
- **Regen after any `code_generator.c` change:**
  `build/recompiler/CdiRecompBios.exe bios/cdi490a.rom --emit` (then rebuild the
  runner). Watch timestamps — a stale recompiler silently regenerates old code.

Build + diff, end to end:
```
# (PowerShell tool) rebuild recompiler -> regen -> rebuild runner
$cm="C:\msys64\mingw64\bin\cmake.exe"; $env:TMP=$env:TEMP="F:\Projects\cdirecomp\build\tmp"
& $cm --build F:/Projects/cdirecomp/build/recompiler -j
& F:\Projects\cdirecomp\build\recompiler\CdiRecompBios.exe bios/cdi490a.rom --emit
& $cm --build F:/Projects/cdirecomp/build/runner -j

# run oracle FIRST (needs steps to fill its ring), then native frozen at fault
Start CdiOracle.exe  bios/cdi490a.rom --steps 250000 --hold      # :4381
Start CdiRuntime.exe bios/cdi490a.rom --fault-hold               # :4380, freezes at fault
python tools/first_divergence.py --start 43161 --context 4
```

- `--fault-hold` freezes the runner at a fatal fault so the rings stay
  queryable (the whole ~159k boot fits in the 262144 ring, so seq 0 survives).
- `first_divergence.py` compares the full register file. **`--start <N>` skips
  capture-timing seams at exception boundaries**: native does the frame push as
  part of the trap while the oracle captures registers pre-push, so A7 mismatches
  for exactly one seq at each trap (benign). 43161 skips the `$40069E` trap seam.
- `tools/cdi_debug.py --port {4380|4381} status | get_registers | trace
  --from S --count N | read_mem --addr A --len L`.

## Tactical backlog (smaller, independent)

- **first_divergence is slow** (a TCP round-trip per 128-record page over ~159k
  insns). A `trace_batch` command or a larger page would speed iteration.
- **Trace ring eviction:** during long interpreter spins the 262144 ring wraps,
  so seq-0 diffing needs a bounded run. A `--max-insns N` cap (clean stop with
  rings intact) would make deep-boot diffing reliable when the boot runs long.
- After MC-CDI-005, the next walls are likely more device handshakes (CDIC/CIAP,
  IKAT, NVRAM) and eventually interrupt DELIVERY (`g_irq_pending` is recorded but
  nothing consumes it) — the OS will want timer/display IRQs to leave idle loops.

## Map of what changed this session (all committed)

- `ca51d4d` oracle driver + first_divergence (Phase B)
- `d26437e` entry-based tracing (boot verified bit-exact, 43,159 insns)
- `373beb7` MC-CDI-011 clean-room hybrid interpreter
- `c2872d1` MCD212 DA timing + UART TxRDY
- `5bee0e3` address-error frame correctness + `--fault-hold`

Key files: `runner/src/m68k_interp.c` (interpreter + the `mcd212_tick(10)` to
fix), `runner/src/mcd212.c` (DA timing), `runner/src/runtime.c`
(`m68k_trap_vector`, `g_fault_opcode`, hybrid wiring), `oracle/cdi_oracle.cpp`
(CeDImu oracle), `recompiler/src/code_generator.c` (odd-JMP emit +
`estimate_cycles` to mirror), `tools/first_divergence.py`.
