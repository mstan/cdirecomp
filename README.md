# cdirecomp — Philips CD-i static recompiler

A static recompiler for the **Philips CD-i**: it lifts the console's
**Motorola 68000-family** machine code (CPU: **SCC68070** @ 15.1049 MHz) into
portable C, then links that generated C against a hand-written runtime that
re-creates the rest of the machine (the **MCD212** video decoder, **CDIC**
CD/ADPCM-audio controller, the **SLAVE** input MCU) and the **CD-RTOS / OS-9**
operating system the games run on. Same spirit and two-tier structure as the
sibling projects: `nesrecomp`, `snesrecomp`, `segagenesisrecomp`, `psxrecomp`,
`virtualboyrecomp`.

**Future game #1: Hotel Mario (USA). Current development scope: BIOS only.**

## Why CD-i is different from the Genesis (read this first)

The Genesis runs a bare-metal 68000 cartridge. **CD-i does not.** A CD-i title
is a Green Book **Mode-2 CD** whose data track holds a CD-i/ISO-9660 file
system; the program is a set of **OS-9/68000 relocatable modules** (sync word
`0x4AFC`) that **CD-RTOS** (a real-time OS based on Microware OS-9/68K v2.4)
loads and relocates into RAM at run time, reaching the system through
`TRAP #0` OS-9 system calls. The whole CD-RTOS system ROM is therefore
recompiled and executed; there is **no hand-written OS-9 HLE layer**. The
clean-room interpreter remains the correctness floor for RAM-resident and
dispatch-missed code, while the hand-written runtime models only hardware.

## Strategy: recompile the BIOS first (like psxrecomp)

CD-i runs CD-RTOS / OS-9 from a player **system ROM**. We do **not** hand-write
HLE stubs for the OS (psxrecomp proved that stubbing the BIOS → silent failure).
We **recompile the BIOS ROM itself**, run it as native C, and let the game's
OS-9 `TRAP #0` calls dispatch into the *recompiled* kernel — kernel state lives
in the emulated RAM, written by recompiled code, no parallel C mirror. We
emulate only the hardware chips. Conveniently, the CD-RTOS ROM is a flat 68000
ROM that boots from its reset vector, so the copied Genesis frontend recompiles
it directly. **The BIOS ROM is user-provided** (copyrighted) — see `bios/`.
We are completing the BIOS/player shell, including navigation, media states,
and real-time behavior **before any Hotel Mario game work begins**. The game
image is currently used only as a real Mode-2 media fixture. Roadmap and
ordering live in **TODO.md**.

## Current status

What is verified today:

- **Shared 68000 frontend** inherited from the author's `segagenesisrecomp`
  (decoder, validator, code generator, function finder, SCC68070 cycle model). Builds clean as
  `CdiRecomp`. See `recompiler/PROVENANCE.txt`.
- **CD-i disc + OS-9 module inventory** (`recompiler/src/disc_parser.c`). Run
  against Hotel Mario it reports: 156100 sectors, MODE2, volume id `"CD-I "`,
  and **203 OS-9 modules** with valid header parity — including the main
  program module **`cdi_hotel`** (Prog, ~110 KB of 68000 code) plus the
  per-level/scene `L0_s01_sub.o … L8_s15_sub.o` modules the game streams off
  the disc.
- **The recompiled CD-RTOS boots to its player-shell STOP** at `$40A3E2` with
  `SR=$2000`, matching CeDImu, with zero dispatch misses. The clean-room hybrid
  interpreter handles RAM-built/dispatch-missed code without AGPL/GPL code in
  the shipped runtime.
- **Genesis-mature overlapping-entry coverage**: 3,982 execution-proven in-ROM
  seeds produce 6,066 functions (5,852 canonical + 214 aliases). Fall-off and
  dispatch audits are clean, true unsupported events are zero, legal
  overlapping 68000 decode streams are retained, and backward cross-boundary
  PC-indexed offset tables resolve through the normal boundary-split path. A
  separate sorted map routes all 49,171 emitted instruction PCs back into their
  canonical native bodies after asynchronous exceptions, without splitting
  those bodies or inflating the callable-function set. OS-9 `TRAP #0` inline
  service words are skipped as data so their post-RTE continuations remain in
  the canonical caller.
- **Cycle-faithful CPU/device time** through the boot. The aligned normal-
  instruction cycle audit is clean through seq 529999, the 43-cycle reset phase
  matches CeDImu from seq 0, and the SCC68070 timer IRQ is accepted on the same
  guest boundary. Player mode advances devices at 60 fps and wakes STOP through
  the real autovector path; fixed-sequence co-sim remains unpaced.
- **Validated differential co-simulation** (`tools/cdi_cosim.py`): full CPU
  state, canonical USP/SSP, incremental full-RAM hashes, cycle traces, injected
  faults, and hash-vs-byte audits. All four trust gates pass.
- **Development observability** (`runner/src/debug_server.c`, `tools/`):
  always-on instruction/store/IRQ/fallback rings and a TCP query surface. The
  `set_input` command is dev instrumentation; it feeds the same timed IKAT path
  as the physical frontend and cannot bypass the IKAT mask.
- **Clean-room MCD212 display pipeline**: ICA/DCA control programs, bitmap,
  CLUT, RGB555, DYUV, RL7/RL3 and mosaic decoding, transparency/matte/ICF
  composition, pixel hold and cursor output. Native completed fields 7–421 are
  byte-identical to CeDImu; field 421 is `768x240`, FNV-1a
  `ddcf263ed1261363`, after an identical 8,152-word ICA/DCA stream. Differential
  frame tracing found and fixed the device-wide Always/Never transparency
  polarity and color-key mask equation. At seq 570000 the earlier boot frame
  also remains exact (`bc0c80cbd59d8383`).
- **Player-facing SDL2 frontend**: resizable 4:3 window, fullscreen toggle,
  keyboard and game-controller mapping. It only consumes atomically published
  frames and feeds the timed IKAT input model, so presentation cannot mutate
  guest state.
- **Real media insertion/ejection boundary**: CUE/BIN images are validated and
  retained as sector backing, then observed by IKAT on emulated time. Mount and
  eject each assert the shell's enabled channel-D IRQ and return cleanly to the
  persistent shell STOP. The smoke dwells in guest frames and rejects any
  observed in-ROM target absent from the trace-guided native seed set. The
  post-resume-map stress passes 30/30 independent Release schedules. Passive
  ready-media detection legitimately issues `E1 00 02 13` reads (first seen at
  field 790 with zero input), polls B0, and remains in the BIOS shell. The
  navigation boundary is therefore proved by button-free input packets,
  persistent shell STOP, and a synthetic media fixture—not by forbidding drive
  reads.
- **Oracle parity** (`oracle/cdi_oracle.cpp`): a local-only headless driver
  serving the matching trace surface on :4381. It is a black-box behavioral
  comparator, never an implementation source or production dependency.
- **Local-only oracle**: an optional, git-ignored CeDImu developer checkout in
  `external/CeDImu` has independent `SCC68070`, `MCD212`, `OS9`, and `HLE`
  implementations used as behavioral reference. CeDImu, local oracle changes,
  and `CdiOracle` are not committed, packaged, or required by `CdiRuntime`.

What is **not** done yet: broad input-driven coverage across every non-launching
player-shell screen, new-CFG coverage for those unexercised paths, and the
remaining BIOS-visible RTC/NVRAM and peripheral behavior. Those are the active
scope. CIAP content delivery, the OS-9 loaded-module bridge, Hotel Mario
modules, and gameplay remain deferred until the BIOS is complete. See
**TODO.md** and **PLAN.md**.

Production releases are deliberately runtime-only: `CdiRuntime` plus explicitly
allowlisted redistributable player dependencies/assets. The recompiler,
`CdiOracle`, CeDImu, clown68000, development tools, user-supplied ROM/disc
images, traces, and build outputs are never packaged. If an AGPL-linked tool is
ever distributed, it will live in a separate tooling repository/release with
complete AGPL compliance; it will not be added to the native player package.
MC-CDI-029 tracks mechanical packaging and native-link enforcement, following
the author-owned release-audit patterns in the sibling `segagenesisrecomp`.
MC-CDI-030's independent rewrite/source audit is complete; see
`PROVENANCE.md`. The obsolete AGPL cycle probe and vendored clown trees are no
longer tracked or referenced by any build target. Existing Git history must be
filtered or replaced by a reviewed clean export before public publication.

## Layout

```
cdirecomp/
├── recompiler/        # CdiRecomp: 68000 frontend (copied) + CD-i disc/module layer
│   ├── src/           #   m68k_decoder/validator/code_generator/... + disc_parser + main_cdi
│   └── PROVENANCE.txt #   what was copied, from where, at which commit
├── runner/            # CdiRuntime: hardware + native/interpreted guest execution
│   ├── include/       #   cdi_runtime.h (the generated-code <-> runner contract), game_extras.h
│   └── src/           #   cdi_bus, runtime, mcd212, cdic, slave, debug_server, main
├── external/          # optional ignored local validation checkouts only
├── hotelmario/        # game.cfg + generated/ (CdiRecomp output)
├── tools/  tests/  docs/
└── PRINCIPLES.md  CLAUDE.md  DEBUG.md  TCP.md  TODO.md
```

## Build

Toolchain: CMake + a C11 compiler + the **SDL2 development package**. The
sibling projects build with **Visual Studio 17 2022**; this scaffold has also
been verified with **MinGW gcc + Ninja**.

```powershell
# Recompiler
cmake -S recompiler -B build/recompiler -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/recompiler -j

# Diagnostic runtime (co-sim instrument enabled)
cmake -S runner -B build/runner -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCDI_COSIM_BUILD=ON
cmake --build build/runner -j

# Production-speed checkpoint
cmake -S runner -B build/runner-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCDI_COSIM_BUILD=OFF
cmake --build build/runner-release -j
```

## Run (today)

```powershell
# Inventory a CD-i disc: tracks, volume descriptor, OS-9 modules
build/recompiler/CdiRecomp.exe "\\SERVER\Games\CDI\Hotel Mario (USA)\Hotel Mario (USA).cue"

# Boot the user-provided system ROM; the normal runtime remains live at STOP
build/runner-release/CdiRuntime.exe bios/cdi490a.rom

# Insert a single-track Mode-2 image at power-on
build/runner-release/CdiRuntime.exe bios/cdi490a.rom --disc game.cue

# Automation/co-sim can explicitly suppress presentation
build/runner/CdiRuntime.exe bios/cdi490a.rom --headless

# Production-paced regression for the persistent shell boundary
py -3 tools/shell_idle_smoke.py build/runner-release/CdiRuntime.exe bios/cdi490a.rom

# Mount/eject regression for a valid Mode-2 CUE/BIN image
py -3 tools/disc_insert_smoke.py build/runner-release/CdiRuntime.exe `
  bios/cdi490a.rom game.cue

# BIOS-only directional navigation; the mounted image is never launched
py -3 tools/bios_navigation_smoke.py build/runner-release/CdiRuntime.exe `
  bios/cdi490a.rom build/tmp/disc_smoke/fixture.cue
```

Player controls: arrows or WASD move the CD-i pointer; Enter, Space, or Z is
button 1; Backspace or X is button 2; F11 or Alt+Enter toggles fullscreen; Esc
closes the player. Standard game-controller D-pad/A/B mappings are also live.
Dropping a CUE or BIN file onto the window performs a real media mount and IKAT
channel-D insertion event.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
