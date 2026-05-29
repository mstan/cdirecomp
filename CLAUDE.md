# CLAUDE.md — cdirecomp project rules

## RULE SOURCE

All rules live in **PRINCIPLES.md** and **DEBUG.md**. This file does not
redefine them; it orients you and pins the project-specific facts.

## FAILURE MODE

If you guess, skip the first divergence, trust unvalidated tool output, or
propose a fix without tracing the writer → the response is INVALID. Restart
from DEBUG.md.

## PROJECT OVERVIEW

Static recompilation of the Philips CD-i (SCC68070, 68000-family) → C → native.
**CD-i runs CD-RTOS / OS-9**: code is OS-9 modules loaded off a Mode-2 CD; the
program reaches the system via `TRAP #0`. **We recompile the CD-RTOS system ROM
itself (no hand-written OS-9 HLE stubs) and run the real OS as native C** — the
game's `TRAP #0` calls dispatch into the recompiled kernel. Recompile the whole
OS to the player shell BEFORE starting the game.

Fixes belong in: the **recompiler** (`recompiler/src/`), the **runner**
(`runner/src/`), or **game.cfg** — NEVER in generated output
(`hotelmario/generated/*.c`).

The 68000 frontend is **shared, copied verbatim** from segagenesisrecomp (see
`recompiler/PROVENANCE.txt`). Keep edits to it minimal and generic; the
long-term plan is to extract it into a module both projects consume
(TODO MC-CDI-009). The only deliberate divergence so far: the emitted
`#include` is `cdi_runtime.h` (not `genesis_runtime.h`).

## ORACLE

- **Behavioral / ground truth:** CeDImu (`external/CeDImu`), the open-source
  CD-i emulator. It has its own SCC68070 / MCD212 / OS9 / HLE cores — read them
  before reimplementing; mirror, don't reinvent.
- **Literal / semantics:** Ghidra over the relevant OS-9 module (68000 mode).
- State which oracle you're using and why before analysis.

## BUILD (OS-first order)

1. Build `CdiRecompBios` + `CdiRecomp`.
2. Recompile the CD-RTOS system ROM: `CdiRecompBios bios/cdi490a.rom --emit`
   → `bios/generated/cdrtos_{full,dispatch}.c`.
3. Build `CdiRuntime` against the recompiled OS; boot to the player shell, diff
   vs CeDImu. Only once that works do we recompile the game (Phase 3).
4. Run native and oracle; compare via the ring buffers / TCP.
5. **Check dispatch misses** — `genesis_log_dispatch_miss` fires when
   `call_by_address` finds no generated function. A miss = a skipped subroutine
   = a silent game-breaking bug. Resolve before any other debugging.

## FILES

- Editable: `recompiler/src/*`, `runner/src/*`, `runner/include/*`,
  `hotelmario/game.cfg`, the device/HLE models.
- NEVER edit: `hotelmario/generated/*.c`.

## TCP

Native debug server on **4380**, oracle on **4381** (+1 convention). See TCP.md.

## CD-i SPECIFICS TO REMEMBER

- Disc: Green Book, Mode-2, 2352-byte raw sectors; Form-1 user data = 2048 B at
  offset 24. Volume descriptor at LBA 16.
- Modules: OS-9/68000, sync `0x4AFC`, header parity word at offset `0x2E`
  (XOR of header words == 0xFFFF). The 24-bit CRC residue check is NOT yet
  verified — don't trust `crc_ok` until MC-CDI-002 lands.
- Hotel Mario boot module: **`cdi_hotel`** (Prog). Levels stream
  `L*_s*_sub.o` modules at run time.
- `TRAP #0` + inline service-code word = OS-9 system call (see
  `runner/src/cdrtos.c`).
