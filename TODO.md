# cdirecomp TODO / roadmap

**Strategy (decided 2026-05-28): recompile the entire OS first — the game comes later.**
BIOS-first, mirroring psxrecomp v4. We statically recompile the whole CD-RTOS /
OS-9 system ROM and get it booting to the player shell **before any Hotel Mario
work begins** (the game is Phase 3 and does not start until Phase 1 boots).
CD-i runs CD-RTOS / OS-9 from a player **system ROM** (the "BIOS"). We do NOT
hand-write HLE stubs for OS-9 — psxrecomp learned the hard way that stubbing the
BIOS (faking syscall outputs, mirroring kernel state in C) causes silent drift
and failure. Instead we **statically recompile the BIOS ROM itself**, run it as
native C, and let the game's OS-9 `TRAP #0` calls dispatch into the *recompiled*
kernel. Kernel state (process/memory/IO tables) lives in the emulated RAM array,
written by recompiled kernel code — no parallel C mirror. We emulate only the
hardware chips (MCD212 video, the CD interface, input, 68070 on-chip
peripherals).

Key realization: **the CD-RTOS ROM is a flat 68000 ROM that boots from its reset
vector — exactly what the copied Genesis frontend already recompiles.** So the
BIOS phase reuses the existing engine; the CD-i-specific work (disc, OS-9 module
loading) comes after the OS boots.

IDs are referenced from code comments (`TODO MC-CDI-NNN`).

## Phase 0 — prerequisite (blocks everything)

- **MC-CDI-019 — Obtain + verify the CD-i system BIOS ROM.** User-provided
  (copyrighted Philips firmware). Target: a CeDImu-known-good **Mono-4** board
  ROM run in **NTSC** mode (Hotel Mario USA), e.g. CDI 470/00, 470/20, 490/00,
  or 220/80. Drop it in `bios/`. Confirm size + reset vector (SSP@$000000,
  PC@$000004) on receipt. See `bios/README.md`.

## Phase 1 — recompile + boot the BIOS (the engine-proof milestone)

- **MC-CDI-001 — BIOS recompiler entry (`main_cdi_bios.c`).** Adapt the flat-ROM
  path (like `main_genesis.c`): load the ROM at its boot base, seed discovery
  from the reset vector + exception vectors + Ghidra-derived OS-9 kernel entries,
  run `function_finder`, emit `bios/generated/cdrtos_full.c` + `_dispatch.c`.
  NO STUBS (port psxrecomp's stub-marker rejection + self-compile check).
- **MC-CDI-020 — Boot-slice proof first.** Before full emission, recompile a
  bounded slice from the reset vector and compile-validate it (psxrecomp Phase
  1a pattern) to prove the engine on real CD-RTOS code.
- **MC-CDI-021 — BIOS seeds file.** Ghidra (68000 mode) over the ROM → kernel /
  syscall-handler / driver entry points. The OS-9 `TRAP #0` handler + F$/I$
  service tables are the priority targets.
- **MC-CDI-016 — Boot to player shell + oracle diff.** Link recompiled BIOS C +
  runtime into one binary (psxrecomp single-binary + shared-dispatch model),
  boot to the CD-i player shell, diff against CeDImu (which boots to shell).
  This is "Hotel Mario" of the BIOS phase: prove the recompiled OS runs.

## Phase 2 — hardware enough to boot the shell

- **MC-CDI-012 — MCD212 video.** Register file + DYUV/RGB555/CLUT/RL7/mosaic
  decoders + plane A/B compositing → ARGB8888. Port from CeDImu (best-supported
  CeDImu device). Needed for the shell to show anything.
- **MC-CDI-022 — Timekeeper (RTC + NVRAM)** at $320000. Required for boot;
  Mono-4 uses 8 KB or 32 KB NVRAM depending on model.
- **MC-CDI-023 — IKAT** (Mono-3/4 input/serial gate; replaces the Mono-2 SLAVE).
  CeDImu HLEs IKAT — mirror that.
- **MC-CDI-006 — SCC68070 on-chip peripherals + MMU** ($80001001..): I2C, UART,
  timers, DMA, MMU translation. CD-RTOS programs these during boot.
- **MC-CDI-010 — exception/trap vectors** beyond OS-9 `TRAP #0`.
- **MC-CDI-011 — hybrid interpreter fallback** for dispatch misses + self-
  modifying code (CD-RTOS writes vectors/stubs into RAM at boot — psxrecomp's
  dirty-RAM interpreter pattern; candidate engine: clown68000 or CeDImu's
  SCC68070 interpreter).

## Phase 3 — the game (Hotel Mario)

- **MC-CDI-024 — OS-9 module loader bridge.** The recompiled CD-RTOS `F$Load`
  relocates a module into RAM; we register that module's *statically recompiled*
  functions into the dispatch table at the relocated base (the novel CD-i piece:
  static recomp of dynamically-loaded modules). Map (module, offset) → recompiled
  function; resolve relocated addresses through the dispatch trampoline.
- **MC-CDI-025 — Recompile the game modules.** `cdi_hotel` (boot/main) first,
  then the streamed `L*_s*_sub.o` level modules. Feeds on the disc parser (done)
  + MC-CDI-024.
- **MC-CDI-013 — CD interface (MCD221 CIAP on Mono-4).** Sector DMA off the disc
  + CD-i ADPCM audio (levels A/B/C) + interrupts. Reuse the `disc_parser` sector
  model. (Note: was scaffolded as "CDIC"; Mono-4 uses CIAP — reconcile.)

## Cross-cutting

- **MC-CDI-002 — Fix OS-9 module CRC-24** (header parity already validates all
  203 Hotel Mario modules; CRC residue check is wrong — validate against them).
- **MC-CDI-003 — Walk the CD-i/ISO-9660 directory** to bound module discovery.
- **MC-CDI-004 — Flat 68070 address model** for the recompiled code (BIOS base +
  module load bases). Mostly resolved by the BIOS-first approach.
- **MC-CDI-005 — SCC68070 cycle timing** refinement (68070 ≠ base 68000 + CD-i
  wait states).
- **MC-CDI-007 — frame pacing / interrupts** (MCD212 display-line IRQ; fiber
  model like the Genesis runner).
- **MC-CDI-015 — TCP debug server + full ring buffer** (frame + reverse-debug
  tiers), per TCP.md. Native 4380 / oracle 4381.
- **MC-CDI-017 — Convert `external/CeDImu` to a git submodule** at first commit.
- **MC-CDI-026 — Behavioral oracle for gameplay.** CeDImu's disc/audio devices
  are incomplete, so it is the reference + BIOS-shell oracle; evaluate MAME
  `cdimono` (more complete) as the in-game behavioral oracle.

## Long-term

- **MC-CDI-009 — Extract the shared m68k frontend module.** The 68000
  decoder/validator/code-generator/function-finder is copied verbatim into BOTH
  segagenesisrecomp and cdirecomp. Once both stabilise, pull it into a standalone
  module (e.g. `m68krecomp-core`) both consume; neutralise inherited `genesis_*`
  names and parameterise the flat-image type. *(User-requested long-term goal.)*
