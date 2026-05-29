# bios/ — CD-i system ROM (you provide this)

This project recompiles the CD-i **system BIOS** (the CD-RTOS / OS-9 player ROM)
as its first milestone — see TODO.md "Strategy: BIOS-first". That ROM is
**copyrighted Philips player firmware**; it is not redistributed here and is not
downloaded automatically. Dump it from a player you own, or source it yourself,
and drop it in this folder.

## What to get

- A CeDImu **known-good** player ROM, run in **NTSC** mode (Hotel Mario is USA).
- Recommended board: **Mono-4** (best emulator + reference support). Known-good
  Mono-4 ROMs per CeDImu's compatibility list:
  - CDI 470/00 (Mono-4, 8 KB NVRAM)
  - CDI 470/20 (Mono-4, 8 KB NVRAM)
  - CDI 490/00 (Mono-4, 32 KB NVRAM)
  - CDI 220/80 (Mono-4, 32 KB NVRAM)
  - (Mono-3 also works: CDI 210/40, 220/60.)
- See CeDImu's README "Compatibility" (`../external/CeDImu/README.md`) and the
  ICDIA player comparison (http://icdia.co.uk/players/comparison.html) for the
  exact board/NVRAM settings each ROM needs.

## Where to put it

Drop the raw ROM here, e.g. `bios/cdi_mono4.rom`, and point the recompiler at it.
The recompiler treats it as a flat 68000 ROM booting from its reset vector
(supervisor SP at $000000, initial PC at $000004), exactly like a Genesis ROM —
so the existing frontend handles it.

## Notes

- CD-i player ROMs are typically ~512 KB–1 MB; we confirm the exact size + base
  + vectors from your dump (MC-CDI-019).
- The board choice fixes the device layer: Mono-3/4 use **MCD221 CIAP** (CD
  interface) and **IKAT** (input), not the Mono-1/2 **CDIC**/**SLAVE**. The
  runtime device stubs will be reconciled to the chosen board.
- This ROM is also what the oracle (CeDImu, and later MAME `cdimono`) needs.
