# cdirecomp — Philips CD-i static recompiler

A static recompiler for the **Philips CD-i**: it lifts the console's
**Motorola 68000-family** machine code (CPU: **SCC68070** @ 15.5 MHz) into
portable C, then links that generated C against a hand-written runtime that
re-creates the rest of the machine (the **MCD212** video decoder, **CDIC**
CD/ADPCM-audio controller, the **SLAVE** input MCU) and the **CD-RTOS / OS-9**
operating system the games run on. Same spirit and two-tier structure as the
sibling projects: `nesrecomp`, `snesrecomp`, `segagenesisrecomp`, `psxrecomp`,
`virtualboyrecomp`.

**Game #1: Hotel Mario (USA).**

## Why CD-i is different from the Genesis (read this first)

The Genesis runs a bare-metal 68000 cartridge. **CD-i does not.** A CD-i title
is a Green Book **Mode-2 CD** whose data track holds a CD-i/ISO-9660 file
system; the program is a set of **OS-9/68000 relocatable modules** (sync word
`0x4AFC`) that **CD-RTOS** (a real-time OS based on Microware OS-9/68K v2.4)
loads and relocates into RAM at run time, reaching the system through
`TRAP #0` OS-9 system calls. So this project is a **hybrid**: the bare-metal
68000 recompiler (shared with the Genesis) **plus a CD-RTOS HLE layer** — much
closer in shape to `xbox-hle` than to the Genesis. The OS surface, not the CPU,
is the real engineering.

## Strategy: recompile the BIOS first (like psxrecomp)

CD-i runs CD-RTOS / OS-9 from a player **system ROM**. We do **not** hand-write
HLE stubs for the OS (psxrecomp proved that stubbing the BIOS → silent failure).
We **recompile the BIOS ROM itself**, run it as native C, and let the game's
OS-9 `TRAP #0` calls dispatch into the *recompiled* kernel — kernel state lives
in the emulated RAM, written by recompiled code, no parallel C mirror. We
emulate only the hardware chips. Conveniently, the CD-RTOS ROM is a flat 68000
ROM that boots from its reset vector, so the copied Genesis frontend recompiles
it directly. **The BIOS ROM is user-provided** (copyrighted) — see `bios/`.
We bring the OS up to the player shell **before any Hotel Mario work begins**
(the game is Phase 3). Roadmap and ordering live in **TODO.md**.

## Current status (scaffold)

What already works and is verified on the real disc:

- **Shared 68000 frontend** copied verbatim from `segagenesisrecomp` (decoder,
  validator, code generator, function finder, cycle probe). Builds clean as
  `CdiRecomp`. See `recompiler/PROVENANCE.txt`.
- **CD-i disc + OS-9 module inventory** (`recompiler/src/disc_parser.c`). Run
  against Hotel Mario it reports: 156100 sectors, MODE2, volume id `"CD-I "`,
  and **203 OS-9 modules** with valid header parity — including the main
  program module **`cdi_hotel`** (Prog, ~110 KB of 68000 code) plus the
  per-level/scene `L0_s01_sub.o … L8_s15_sub.o` modules the game streams off
  the disc.
- **Runtime substrate** (`runner/`): SCC68070 memory map (grounded in CeDImu's
  Mono2 bus), MMIO routing, OS-9 `TRAP #0` gateway, SCC68070 exception model,
  dispatch-miss accounting. Builds clean as `CdiRuntime`. Everything not yet
  modelled **fails loud** (no silent stubs). Booting `cdi490a.rom` runs ~43k
  recompiled instructions before reaching a RAM-resident stub (dispatch miss at
  `$050A`, the hybrid-interpreter milestone MC-CDI-011).
- **Observability** (`runner/src/debug_server.c`, `tools/`): always-on block-
  trace ring (262144 blocks: PC + full register file) + frame ring, a fault
  trail dumped on every abort, and a threaded TCP debug server (127.0.0.1:4380)
  answering `ping/status/get_registers/read_mem/trace/dispatch_miss_info`.
  Clients: `tools/cdi_debug.py`, `tools/check_dispatch_misses.py` (RULE 0a).
- **Oracle parity** (`oracle/cdi_oracle.cpp`): a headless driver linking the
  wxWidgets-free CeDImu core, serving the SAME trace surface on :4381.
  `tools/first_divergence.py` pages both rings and reports the first divergence.
  Result: the recompiled CD-RTOS is **bit-exact to the CeDImu oracle across all
  43,159 boot instructions** (full register file, every instruction sampled at
  entry) — zero codegen divergence. Native stops only because it reaches the
  `$050A` RAM-built stub (dispatch miss); the oracle's interpreter walks into it.
  That RAM stub is the structural boot blocker (MC-CDI-011 hybrid interpreter).
- **Oracle**: CeDImu (open-source C++ CD-i emulator) cloned into
  `external/CeDImu` — has its own `SCC68070`, `MCD212`, `OS9`, and `HLE`
  implementations we use as the reference + future in-process oracle.

What is **not** done yet (the actual project): the CD-RTOS/OS-9 loader that
turns modules into runnable code, the MCD212/CDIC/SLAVE device models, and the
oracle wiring. See **TODO.md** (`MC-CDI-*` roadmap).

## Layout

```
cdirecomp/
├── recompiler/        # CdiRecomp: 68000 frontend (copied) + CD-i disc/module layer
│   ├── src/           #   m68k_decoder/validator/code_generator/... + disc_parser + main_cdi
│   └── PROVENANCE.txt #   what was copied, from where, at which commit
├── runner/            # CdiRuntime: CD-i hardware + CD-RTOS HLE
│   ├── include/       #   cdi_runtime.h (the generated-code <-> runner contract), game_extras.h
│   └── src/           #   cdi_bus, runtime, mcd212, cdic, slave, cdrtos, debug_server, main
├── external/          # clown68000 (cycle probe), clowncommon, CeDImu (oracle)
├── hotelmario/        # game.cfg + generated/ (CdiRecomp output)
├── tools/  tests/  docs/
└── PRINCIPLES.md  CLAUDE.md  DEBUG.md  TCP.md  TODO.md
```

## Build

Toolchain: CMake + a C11 compiler. The sibling projects build with **Visual
Studio 17 2022**; this scaffold has also been verified with **MinGW gcc +
Ninja**.

```powershell
# Recompiler
cmake -S recompiler -B build/recompiler -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/recompiler -j

# Runtime
cmake -S runner -B build/runner -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/runner -j
```

## Run (today)

```powershell
# Inventory a CD-i disc: tracks, volume descriptor, OS-9 modules
build/recompiler/CdiRecomp.exe "\\SERVER\Games\CDI\Hotel Mario (USA)\Hotel Mario (USA).cue"

# Bring up the runtime substrate (reports honestly that there's nothing to run yet)
build/runner/CdiRuntime.exe "Hotel Mario (USA).cue"
```

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
