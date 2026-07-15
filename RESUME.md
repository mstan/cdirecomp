# RESUME.md — pick up here next session

Last updated 2026-07-15. `HANDOFF.md` carries the detailed historical record;
this file is the short active-state guide.

## Accepted boundary

- The recompiled CD-RTOS BIOS boots to its persistent player shell with zero
  dispatch misses. BIOS navigation, storage/settings, battery persistence,
  captured-mouse input, and one-shot host RTC sync are closed.
- The real shell launches a user-supplied Hotel Mario (USA) CUE/BIN. Loaded
  game modules currently execute through the project-owned clean-room hybrid
  fallback; no CeDImu, clown68000, or AGPL code is linked or required.
- The Hotel Mario attract gate reaches the later animated intro beyond LBA
  4650. It requires the pixel-exact Fantasy Factory title, a populated plane-B
  background palette/scanline, clean bumper and intro XA audio with zero drops,
  zero native dispatch misses, and at least 55 fields/second.
- The background-loss bug was CIAP routing, not MCD212 decoding: EOF/EOR/trigger
  sectors must still satisfy file/channel selection. Channel-0 zero-filled
  record tails had been entering the selected channel-15 video stream.
- `F:\Projects\HotelMarioRecomp` is the thin per-game project. Its executable
  requires both BIOS and `--disc`; its launcher persists ignored `bios.cfg` and
  `disc.cfg` paths. No copyrighted assets or external emulator sources belong
  in that repository.

## Next critical path

1. Promote the observed relocated Hotel Mario modules into the static native
   tier (MC-CDI-024/025), using the accepted attract gate as the floor.
2. Measure and reduce interpreter fallback without changing video/audio/timing.
3. Begin gameplay and full-playthrough certification; expand CIAP/peripheral
   coverage only when a real application path requires it.

## Verification

```powershell
cmake --build build/runner-release --config Release

build/runner-release/CdicTest.exe
build/runner-release/CdiAudioTest.exe
build/runner-release/Mcd212VideoTest.exe

py -3 tools/hotelmario_launch_probe.py `
  build/runner-release/CdiRuntime.exe bios/cdi490a.rom game.cue `
  --seconds 55 --timeout 180 --quick --compact --accept-attract

F:\Projects\HotelMarioRecomp\tools\build.ps1
```

Keep `external/CeDImu` ignored and local. Production/player releases remain
runtime-only; existing historical vendor blobs require a reviewed clean export
or history rewrite before this repository is made public.
