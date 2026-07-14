# Session Handoff — cdirecomp

**Date:** 2026-07-13
**Goal:** a fully navigable, real-time, LLE-focused BIOS/player shell. Hotel
Mario is only a real-media fixture; game launch and gameplay are out of scope
until the BIOS goal is complete.

## Non-negotiable architecture

- Recompile and run the whole CD-RTOS/OS-9 ROM. Never replace OS services with
  hand-written HLE stubs.
- Keep the clean-room hybrid interpreter for RAM-resident and dispatch-missed
  guest code. Do not link clown68000/AGPL or CeDImu/GPL into the shipped runtime.
- CeDImu is the behavioral oracle; Ghidra 68000 mode is the literal oracle.
- Never edit generated C. Codegen changes require BIOS regeneration.
- Debug only through the always-on rings/TCP surface. Free-run observers and
  query their rings; never pause/step two observers into apparent alignment.
- RULE 0a: resolve dispatch misses before other runtime debugging.

## Verified state

- Phase C is complete: native reaches `STOP #$2000` at `$40A3E2`, `SR=$2000`,
  matching CeDImu, with no `$FFFFFF` continuation and zero dispatch misses.
- Genesis-style overlapping-entry ownership is active. The trace-guided BIOS
  set has 3,982 in-ROM entries; codegen emits 6,066 functions (5,852 canonical
  + 214 aliases), fall-off/dispatch audits are clean, and unsupported events
  are zero. The two legal `$41246E/$412472` overlapping decode entries are
  emitted as canonical streams and surfaced by a separate structural audit.
- Because IRQ/bus-error delivery abandons generated host frames, codegen also
  emits a sorted native resume map for all 49,171 emitted instruction PCs.
  Seventeen PCs where legal overlapping streams converge are assigned to the
  established exact-entry owner (or deterministic lower host). This does not
  split functions or manufacture callable entries.
- Async exception resume PCs feed the same coverage loop as missed indirect
  calls/jumps. Paired PC-indexed `MOVE.W`/`JMP` offset tables now discover
  backward cross-boundary handlers through the normal boundary-split path; the
  dispatch audit has 14 offset tables and zero unresolved interiors.
- Repeated real-media schedules previously exposed stochastic interior resume
  PCs and drove the seed list upward. The async native resume map fixes that
  general class: `indirect_targets` now reports only RAM or genuinely unknown
  ROM CFGs. The same Release stress that previously failed 10 of 30 schedules
  now passes 30/30 with no new entries. This is still bounded evidence, not a
  substitute for input-driven navigation coverage.
- Nested generated RTE/RTR now propagates through every C JSR frame to the
  depth-zero trampoline; skip-RTS remains a one-frame unwind. The post-STOP
  service window is full-state identical to CeDImu for 54,773 native
  instructions (135 benign wait-loop re-syncs, maximum skew 2).
- Native and CeDImu have identical absolute cycle time from seq 0 onward: the
  native reset entry now carries the SCC68070 reset exception's 43 cycles. The
  aligned normal-instruction audit is clean through seq 529999, and the full
  CPU/RAM realigner reports no true divergence across `[331000,592896)` (484
  benign one-instruction re-syncs). Co-sim's four trust gates all pass.
- The SCC68070 timers now mirror CeDImu's 96-cycle prescaler, T0/T1/T2,
  TCR/TSR/RR/PICR1 behavior and on-chip level delivery. A missing combined-mask
  check at the universal instruction safepoint was fixed; timer IRQs are raised
  and accepted on the same guest boundary as CeDImu.
- STOP is now persistent and faithful: the CPU advances devices in CeDImu's
  25-cycle stopped quanta and clears STOP only when an eligible IRQ is taken.
- Player-mode CPU/device time is host-paced at the 15.1049 MHz SCC68070 clock,
  including the active ISR/shell bursts between STOPs. Fixed-sequence co-sim
  disables pacing and remains deterministic. The shell smoke measures 60 fps.
- IKAT has the 25-ms Class::Maneuvering packet cadence on channel A, matching
  the literal cdi490 `pt1driv` wiring (CeDImu's Mono3 HLE uses incompatible B).
  Host state enters through the transport-neutral `CDI_INPUT_*` API. TCP
  `set_input` is development instrumentation only, not the player frontend.
- At the no-disc shell STOP, IKAT `IMR=$A0`: CD-RTOS enables channels C/D and
  masks unopened pointing-device channel A. A button event queues four bytes
  and asserts ISR `$02` but does not bypass the mask or raise level 2.
- `tools/shell_idle_smoke.py` verifies all of the above end to end, including
  real-time pacing (60.00 fps in the latest Release run) and proof that a
  masked channel-A packet does not assert the IKAT level-2 line.
- The clean-room MCD212 pipeline executes ICA/DCA and publishes complete ARGB
  frames. Native and CeDImu completed fields are byte-identical throughout
  `[7,422)`; field 421 is 768x240 with FNV-1a `ddcf263ed1261363`, and both sides
  executed the same 8,152 ICA/DCA words (FNV-1a `87e67699d73fc5af`). The first
  two pixel divergences were general renderer bugs: TC=0/8 Always/Never
  transparency polarity and the color-key mask's OR-mask equation.
- The SDL2 player frontend presents those completed frames without vsync stalls
  and feeds keyboard/gamepad input through the same timed IKAT path. CUE/BIN
  drag-and-drop mounts real media.
- `tools/disc_insert_smoke.py` mounts a valid Mode-2 image only as a media
  fixture after shell STOP. It proves insertion and ejection
  each raise exactly one enabled IKAT channel-D IRQ, dwells in guest frames,
  requires persistent RULE 0a-clean shell return, and fails on any observed
  in-ROM entry absent from `bios/cdrtos_discovered.txt`. An untouched ready-media
  shell does issue `E1 00 02 13` at field 790 and later repeats while detecting
  media, then polls B0 and remains at STOP; drive reads are not evidence of game
  launch.
- Ready media uses `B0 00 02 15`, matching the ROM handler's byte fields. It
  opens `pt1driv` (`IMR=$A2`). `tools/bios_navigation_smoke.py` decodes and
  validates signed LEFT/DOWN/UP/RIGHT reports, proves each channel-A IRQ and
  guest drain, observes hardware-cursor and framebuffer movement, and returns
  RULE 0a-clean to STOP without a button packet. Five consecutive Release runs
  pass with a synthetic fixture; no application was launched.
- The resulting ROM targets `$43FD2E/$43FD54/$440022` exposed a general OS-9
  CFG hole: each follows `TRAP #0` plus an inline service word. The recompiler
  now follows those continuations inside their canonical callers. Regeneration
  emits 6,066 functions (5,852 canonical + 214 aliases) and a 49,171-entry
  async resume map; the media coverage smoke is dry without new seeds.
- Debug/co-sim build remains deterministic and all co-sim gates pass at 20k.
  Gate 4 now waits for and validates every requested checkpoint before sampling;
  both native and oracle pass at exact seq 5k/10k/15k/20k.
- Historical unpaced checkpoint: Release with `CDI_COSIM_BUILD=OFF`, full MCD212 pixel
  output active, reaches the shell STOP in 0.522–0.535 s over three warm runs.
  The pixel path uses an ICF lookup table and forward-only matte walk; the
  seq-570000 frame hash remains exact. Player runs are now intentionally
  real-time paced, so boot wall time is no longer a raw throughput benchmark.

## Current working tree

Changes are intentionally uncommitted. They include the Phase-C cycle/co-sim
work, the hybrid return-contract repair, SCC68070 timers and on-chip IRQs,
persistent STOP, player pacing, IKAT input/media, display/frontend work, smoke
tests, and documentation. Inspect `git diff`; do not discard user changes.

Key modified/new files this session:

- `recompiler/src/code_generator.c`, `runner/src/runtime.c`: preserve the hybrid
  interpreter/native return contract across re-entry; implement correct
  register-count ROXL/ROXR.L, backward cross-boundary PC-indexed offset-table
  discovery, and native owner-routed resume at every emitted instruction PC;
  BIOS was regenerated.
- `bios/cdrtos_discovered.txt`, `runner/src/runtime.c`,
  `tools/collect_seeds.py`: dry trace-guided indirect/resume coverage loop.
- `recompiler/src/codegen_diag.c`, `tools/test_function_aliases.py`: separate
  supported overlapping decode streams from true unsupported events and cover
  alias ownership plus nested RTE propagation synthetically.
- `runner/src/periph.c`, `runner/src/mcd212.c`, `runner/src/debug_server.c`:
  SCC68070 timers, reset-cycle/device phase, combined external/on-chip IRQ
  delivery, and high-resolution whole-player cycle pacing.
- `runner/src/main.c`: persistent STOP/IRQ wake and `--exit-on-stop` diagnostic.
- `runner/src/slave.c`, `runner/include/cdi_runtime.h`: timed maneuvering-device
  packets, common input ABI, media-status transitions and IKAT event ring.
- `runner/src/mcd212.c`, `runner/src/mcd212_video.c`: line/control timing,
  ICA/DCA, decode/composition, cursor, and canonical framebuffer publication.
- `runner/src/cdi_frontend.c`, `runner/src/cdi_media.c`: SDL presentation/
  physical input plus synchronized real CUE/BIN media backing.
- `runner/src/debug_server.c`, `tools/cdi_debug.py`, `TCP.md`: dev-only input
  injection, side-effect-free `emu_ikat_state`, and query-triggered immediate
  pause at the next already-recorded trace entry.
- `tools/shell_idle_smoke.py`: persistent-shell/pacing/masked-channel-A regression.
- `tools/disc_insert_smoke.py`: source-specific channel-D media/IRQ regression.
- `tools/bios_navigation_smoke.py`: ready-media guest-driver/UI navigation regression.
- `README.md`, `PLAN.md`, `TODO.md`: current milestone and remaining work.

## Next critical path

1. Cover the remaining non-launching player-shell screens and BIOS-visible
   settings while keeping every input button boundary explicit and RULE 0a clean.
2. Audit memory/settings UI behavior plus RTC/NVRAM and peripheral behavior
   without activating Play CD-I.
3. Add the fixed guest-clock RAM/register baseline, SCC68070 instruction-
   coverage audit, and CI. Game
   loading, module recompilation, and gameplay remain deferred.

## Rebuild and verification

```powershell
$env:Path = "C:\msys64\mingw64\bin;" + $env:Path
$env:TMP  = "F:\Projects\cdirecomp\build\tmp"; $env:TEMP = $env:TMP
$cm = "C:\msys64\mingw64\bin\cmake.exe"

# Recompiler/codegen changes only: rebuild, regenerate BIOS, then runner.
& $cm --build F:/Projects/cdirecomp/build/recompiler -j
& F:/Projects/cdirecomp/build/recompiler/CdiRecompBios.exe `
  F:/Projects/cdirecomp/bios/cdi490a.rom --emit
& $cm --build F:/Projects/cdirecomp/build/runner -j
& $cm --build F:/Projects/cdirecomp/build/oracle -j

py -3 tools/cdi_cosim.py --self-test
py -3 tools/cdi_cosim.py gates build/runner/CdiRuntime.exe `
  build/oracle/CdiOracle.exe bios/cdi490a.rom 20000
py -3 tools/shell_idle_smoke.py build/runner/CdiRuntime.exe `
  bios/cdi490a.rom --port 4396

# Production-speed build/checkpoint.
& $cm -S F:/Projects/cdirecomp/runner `
  -B F:/Projects/cdirecomp/build/runner-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCDI_COSIM_BUILD=OFF
& $cm --build F:/Projects/cdirecomp/build/runner-release -j
& build/runner-release/CdiRuntime.exe bios/cdi490a.rom --exit-on-stop
```

Always kill held instances before rebuilding or launching:

```powershell
Get-Process CdiRuntime,CdiOracle -ErrorAction SilentlyContinue |
  Stop-Process -Force
```
