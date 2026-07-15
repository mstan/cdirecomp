# Source provenance

This record covers the source that builds `CdiRuntime` and the author-owned
`CdiRecomp` frontend. It distinguishes implementation inputs from optional
black-box validation tools.

## Implementation basis

| Component | Independent implementation basis | Project evidence |
|---|---|---|
| 68000 decode, code generation, and interpreter | Author-owned `segagenesisrecomp` frontend ancestry; Motorola 68000 architecture and SCC68070 timing/exception documentation | `recompiler/PROVENANCE.txt`, `runner/tests/m68k_arith_test.c`, generated-code differential tests |
| SCC68070 exception frames, timers, interrupt controller, and UART | SCC68070 User Manual, especially exception processing and sections 2.13.1–2.13.12 | `runner/tests/periph_test.c`, BIOS boot and co-simulation gates |
| DS1216 phantom clock/NVRAM | Analog Devices DS1216 data sheet: serial key, register layout, oscillator, BCD calendar, and SRAM pass-through | `runner/tests/cdi_nvram_test.c` |
| MCD212 timing, control programs, and video | Philips CD-i Full Functional Specification / Green Book register and image-coding descriptions | `runner/tests/mcd212_video_test.c`, frame hashes, BIOS boot |
| IKAT input controller | Public CD-i register descriptions plus project-owned BIOS bus traces and scheduled-input tests | navigation and disc-insert smoke tests |
| CIAP CD interface | BIOS register probes and project-owned access/event traces | disc-insert and BIOS navigation smoke tests |
| CD-i address map and device wiring | Mono-I board documentation, hardware manuals, and BIOS access probes | bus diagnostics, BIOS boot, co-simulation state hashes |

Specification locations used during the rewrite:

- SCC68070 User Manual: <https://d-nb.info/880525312/04>
- Analog Devices DS1216 product page/data sheet: <https://www.analog.com/en/products/ds1216.html>
- ICDIA CD-i technical-document catalog: <https://www.icdia.co.uk/techdocs/>

The device implementations above were rewritten without copying third-party
emulator source. Optional emulators may be run as black-box behavioral
comparators; their output is test evidence, not implementation authority.

The 2026-07-14 final audit found no exact sequence of 24 or more code tokens
shared between project source and either local third-party checkout. Validation
aligned 659,998 near-full-boot instruction transitions with zero skips,
resynchronizations, timing mismatches, or cumulative cycle drift; all focused
unit tests and Release shell/media/navigation smokes passed.

## Excluded third-party tools

CeDImu is an optional, git-ignored local oracle checkout. Its source, local
patches, and resulting `CdiOracle` binary are not part of this repository's
player or recompiler targets and are never packaged.

The formerly vendored AGPL clown68000/clowncommon trees and their cycle-probe
adapter were removed on 2026-07-14. Neither `CdiRuntime` nor `CdiRecomp` now
includes, links, or requires them. Local historical checkouts remain ignored.

Production packaging is runtime-only. Before publishing this repository's
existing Git history, remove the historical vendor blobs from that history or
publish from a reviewed clean export; deleting them from the current tree does
not erase earlier commits.

## Audit rule

Any future production implementation must cite a hardware specification,
author-owned ancestor, or project-owned experiment/test. Third-party source may
be isolated as a separately licensed development tool, but it must not be used
as source text for the player implementation or enter a player/recompiler build.
