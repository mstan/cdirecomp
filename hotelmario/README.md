# Hotel Mario (USA) — notes

Source disc: `\\SERVER\Games\CDI\Hotel Mario (USA)\` (`.bin` 367,147,200 bytes
+ `.cue`). A `(Beta) (1993-11-23)` build and two Zelda CD-i titles sit beside
it on the share for the "second game decouples the hardcoding" step.

## Disc inventory (from `CdiRecomp.exe <cue>`)

- 156100 sectors, raw 2352-byte Mode-2, volume id `"CD-I "`.
- **203 OS-9 modules** with valid header parity and CRC; 174 distinct
  executable image identities after deduplication.

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

## Native generation

The real recompiled CD-RTOS kernel loads and links the original modules.
There are no OS-9 HLE calls or replacement game routines. Generate from your
own disc with:

```powershell
build/recompiler/CdiRecomp.exe "disc/Hotel Mario (USA).cue" --emit --out hotelmario/generated --subr-exports named-offset32
```

The explicit `named-offset32` option validates Hotel Mario's private Subr
export convention: `M$Exec` points to terminated pairs of name and code
offsets. It is not treated as a universal OS-9 Subr format. Program modules
use their executable and exception entry points.

Generated C, the source list and full-image manifest remain local and ignored.
Binding uses the complete image SHA-256, not its name or a fixed RAM address.
RAM writes invalidate bindings, and uncovered instructions remain interpreted.

For callbacks observed during a real run, export `module_targets` using
`tools/collect_module_seeds.py` and regenerate with `--module-seeds <file>`.
Each seed is a SHA-256 plus a module-relative instruction offset; regeneration
fails for unknown images, metadata/CRC offsets, unsupported instructions or
unterminated native code. This is an offline compilation step.

See [ISSUES.md](../ISSUES.md) for tested boundaries and the remaining campaign
work. LLE remains the default; HLE optimizations require a working LLE floor
and explicit developer opt-in under the shared template's HLE rules.
