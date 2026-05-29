# Hotel Mario (USA) — notes

Source disc: `\\SERVER\Games\CDI\Hotel Mario (USA)\` (`.bin` 367,147,200 bytes
+ `.cue`). A `(Beta) (1993-11-23)` build and two Zelda CD-i titles sit beside
it on the share for the "second game decouples the hardcoding" step.

## Disc inventory (from `CdiRecomp.exe <cue>`)

- 156100 sectors, raw 2352-byte Mode-2, volume id `"CD-I "`.
- **203 OS-9 modules** with valid header parity.

## Module structure

| Module | Type | Notes |
|--------|------|-------|
| `cdi_hotel` | Prog | **Boot/main program** (~112984 B @ LBA 2272) — first recompilation target |
| `cdi_hotel_data`, `cdi_hotel.stb`, `cdi_bumpdata` | Data | main program data |
| `cdi_bumper` | Prog | secondary program (~11614 B) |
| `intro_sub.o`, `mario_set_sub.o` | Subr | intro + Mario set-up code |
| `L0_s01_sub.o` … `L8_s15_sub.o` | Subr | **per-level/scene code, streamed at run time** (each with a `_subT.o` twin) |
| `cdi_L*_dat.map`, `cdi_L*_av.map`, `cdi_L*_am.map` | Data | per-level data / AV maps |

The level/scene `*_sub.o` modules confirm Hotel Mario streams gameplay code off
the disc as you progress (classic CD-i `F$Load` model) — the CD-RTOS loader
(MC-CDI-001) and CDIC (MC-CDI-013) are what make that work.

## First milestone

Recompile `cdi_hotel`, get CD-RTOS init + the first OS-9 calls HLE'd, and reach
the point where the MCD212 is programmed for the intro — diffing every step
against CeDImu.
