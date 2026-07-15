# cdirecomp TODO / roadmap

**Strategy (decided 2026-05-28): recompile the entire OS first — the game comes later.**
BIOS-first, mirroring psxrecomp v4. We statically recompile the whole CD-RTOS /
OS-9 system ROM and make the player shell fully navigable and real-time **before
any Hotel Mario game work begins**. The Hotel Mario image may be mounted only
as a real-media BIOS fixture; game launch and gameplay remain deferred.
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

- ✅ **MC-CDI-012 — MCD212 video.** Clean-room register/timing and ICA/DCA
  sequencer plus DYUV/RGB555/CLUT/RL7/RL3/mosaic decoders, transparency,
  matte/ICF/mixing, pixel hold and cursor composition → double-buffered
  ARGB8888. Completed fields `[7,422)` are byte-identical to CeDImu, including
  identical ICA/DCA event streams; field 421 hashes to `ddcf263ed1261363`.
  Semantics were checked against the Motorola MCD212 manual; CeDImu remains the
  behavioral output oracle.
- **MC-CDI-022 — Timekeeper (RTC + NVRAM)** at $320000. Required for boot;
  Mono-4 uses 8 KB or 32 KB NVRAM depending on model.
- **MC-CDI-023 — IKAT** (Mono-3/4 input/serial gate; replaces the Mono-2 SLAVE).
  CeDImu HLEs IKAT — mirror that. Command/response channels plus the timed
  25-ms Class::Maneuvering channel-A packet generator and physical SDL keyboard/
  game-controller mapping are present. At the no-disc shell STOP, CD-RTOS uses
  `IMR=$A0` (channels C/D enabled) and keeps unopened channel A masked, so queued
  input correctly does not assert level 2. With ready media, `pt1driv` enables
  channel A (`IMR=$A2`); the navigation smoke proves IRQ, guest drain, visible
  four-way BIOS movement, framebuffer publication, STOP return, and RULE 0a.
  The ready B0 field layout is `B0 00 02 15`. Passive ready-media detection may
  issue `E1 00 02 13`; the BIOS-only boundary is button-free pointer packets and
  persistent shell execution, not the absence of drive reads.
- **MC-CDI-006 — SCC68070 on-chip peripherals + MMU** ($80001001..): I2C, UART,
  timers, DMA, MMU translation. UART reset/TX state and T0/T1/T2 with the
  96-cycle prescaler, TCR/TSR/RR/PICR1 and on-chip IRQ delivery are present;
  I2C, DMA and MMU coverage remain.
- **MC-CDI-010 — exception/trap vectors** beyond OS-9 `TRAP #0`.
- ✅ **MC-CDI-011 — hybrid interpreter fallback.** The clean-room shared-
  decoder interpreter remains the correctness floor for RAM-built vectors and
  stubs. Trace-guided discovery currently contains 3,982 in-ROM entries and
  emits 6,066 functions (5,852 canonical + 214 aliases) with RULE 0a, fall-off,
  unsupported-dispatch, and interior-offset-table audits clean. A separate
  49,171-entry map resumes asynchronous exceptions at every emitted instruction
  PC without splitting canonical functions. Configured OS-9 `TRAP #0` scanning
  skips its inline service word and owns the post-RTE continuation in the
  original caller. Trace discovery now grows only for genuinely unknown CFGs;
  broader BIOS navigation coverage remains open.

## Player-quality enhancements (PLAN Phase D; opt-in, persistent)

- ✅ **MC-CDI-027 — Captured host mouse.** A launcher-compatible, persistent
  `input.capture_mouse` player preference. While the SDL window is focused,
  hide/capture the host pointer and feed true relative X/Y plus primary/
  secondary button transitions through the timed IKAT pointing-device path.
  Losing focus must immediately release the pointer and restore the host
  cursor. Off remains the faithful/default path; scripted, headless, co-sim,
  and baseline profiles keep it disabled. Focus/button unit coverage and the
  BIOS navigation smoke cover clamping/remainder, exact relative deltas, guest
  drain, cursor/framebuffer movement, and RULE 0a.
- ✅ **MC-CDI-028 — One-shot host clock seed.** A launcher-compatible, persistent
  `rtc.sync_host_on_startup` player preference. When enabled, seed the DS1216
  RTC from host-local civil time once before guest execution; then advance only
  on emulated cycles and honor all guest writes without re-syncing. NVRAM policy
  remains independent, and disabled/oracle runs retain the deterministic
  1989 seed. Unit coverage proves cycle advancement and no re-seed after guest
  writes; `tools/rtc_startup_smoke.py` proves the real persistent startup path.

These preferences are user/player configuration, not title policy in
`game.cfg`. The shared `player.cfg` load/save contract survives process and
launcher restarts; the current runtime creates it in SDL's user preference
folder and a future launcher UI can edit the same file/API.

## Phase 3 — the game (Hotel Mario)

**Deferred:** do not begin this phase until BIOS/player-shell navigation,
real-time performance, and native coverage are closed.
Disc insertion/ejection tests in the BIOS phase are not game progress.

- **MC-CDI-024 — OS-9 module loader bridge.** The recompiled CD-RTOS `F$Load`
  relocates a module into RAM; we register that module's *statically recompiled*
  functions into the dispatch table at the relocated base (the novel CD-i piece:
  static recomp of dynamically-loaded modules). Map (module, offset) → recompiled
  function; resolve relocated addresses through the dispatch trampoline.
- **MC-CDI-025 — Recompile the game modules.** `cdi_hotel` (boot/main) first,
  then the streamed `L*_s*_sub.o` level modules. Feeds on the disc parser (done)
  + MC-CDI-024.
- **MC-CDI-013 — CD interface (MCD221 CIAP on Mono-4).** Real CUE/BIN-backed
  media mount/eject and the enabled IKAT channel-D shell IRQ are present and
  regression-tested. The shell's passive `E1 00 02 13` detection read is visible
  in the IKAT ring, but feeding application content through CIAP sector
  buffers/DMA and CD-i ADPCM audio (levels A/B/C) remains deferred with Phase 3.
  The legacy `cdic_*` entry-point names still route the Mono-4 CIAP region.

## Cross-cutting

- **MC-CDI-002 — Fix OS-9 module CRC-24** (header parity already validates all
  203 Hotel Mario modules; CRC residue check is wrong — validate against them).
- **MC-CDI-003 — Walk the CD-i/ISO-9660 directory** to bound module discovery.
- **MC-CDI-004 — Flat 68070 address model** for the recompiled code (BIOS base +
  module load bases). Mostly resolved by the BIOS-first approach.
- **MC-CDI-005 — SCC68070 cycle timing** refinement (68070 ≠ base 68000 + CD-i
  wait states).
- ✅ **MC-CDI-007 — frame pacing / interrupts.** External and SCC68070 on-chip
  IRQ delivery, faithful STOP wake semantics, MCD212 display/control-program
  interrupts, whole-player wall-clock pacing (including active timer-ISR
  bursts), and non-vsync SDL frame presentation are present.
- **MC-CDI-015 — TCP debug server + full ring buffer** (frame + reverse-debug
  tiers), per TCP.md. Native 4380 / oracle 4381.
- **MC-CDI-017 — Keep CeDImu behind the local-only oracle boundary.**
  `external/CeDImu` remains an optional, git-ignored developer checkout used to
  build `CdiOracle`; neither its source, local modifications, patch set, nor the
  statically linked oracle binary is committed, pushed, packaged, or required
  by `CdiRuntime`. Public documentation may identify the expected upstream
  project/base revision and local setup boundary, but cdirecomp does not carry
  or distribute a CeDImu fork. Audit any production file described as a "port"
  so shipped code has independent/specification-based provenance.
- **MC-CDI-026 — Behavioral oracle for gameplay.** CeDImu's disc/audio devices
  are incomplete, so it is the reference + BIOS-shell oracle; evaluate MAME
  `cdimono` (more complete) as the in-game behavioral oracle.
- ✅ **MC-CDI-029 — Runtime-only release enforcement.** Production packages contain
  `CdiRuntime` plus explicitly allowlisted redistributable runtime dependencies/
  assets only—never the recompiler, `CdiOracle`, CeDImu, clown68000, developer
  tools, ROM/disc images, traces, or build debris. The CD-i-specific
  `tools/package_runtime_release.py` builds from a five-file allowlist, rejects
  unexpected PE imports and oracle/emulator markers, re-audits the final ZIP,
  and emits its SHA-256 checksum. If an
  AGPL-linked tool is ever distributed, create a separate tooling repository
  and satisfy AGPL there; it does not join the player release. The clean-room
  68000 fallback has already been adapted from the sibling; reuse further
  author-owned components only with explicit provenance and CD-i validation.
- ✅ **MC-CDI-030 — Production-source provenance audit.** Independently rewrote
  the flagged SCC68070 exception/DIV/peripheral, IKAT, DS1216, CIAP, and MCD212
  timing/video paths from hardware specifications and project-owned traces.
  Removed the unused AGPL cycle probe and untracked the vendored clown68000 /
  clowncommon trees; no recompiler or runtime target references them. Added
  focused DS1216, peripheral, MCD212-video, and DIVU/DIVS tests plus
  `PROVENANCE.md`. The final audit found no exact run of 24 or more code tokens
  shared with either local third-party checkout; 659,998 near-full-boot
  instruction transitions retain zero resyncs, cost mismatches, or cycle drift.
  Existing Git history still contains the removed vendor blobs and must be
  filtered or replaced by a clean export before that history is made public.

## Long-term

- **MC-CDI-009 — Extract the shared m68k frontend module.** The 68000
  decoder/validator/code-generator/function-finder is copied verbatim into BOTH
  segagenesisrecomp and cdirecomp. Once both stabilise, pull it into a standalone
  module (e.g. `m68krecomp-core`) both consume; neutralise inherited `genesis_*`
  names and parameterise the flat-image type. The author explicitly permits
  salvaging their clean-room tooling/interpreter work from
  `../segagenesisrecomp`; exclude its clownmdemu/clown68000 oracle paths and
  retain file/commit provenance. *(User-requested long-term goal.)*
