# cdirecomp 0.0.1 internal runtime checkpoint

This is a private, BIOS-only Windows x64 checkpoint of the Philips CD-i static
recompilation runtime. It is not yet a game-ready public release.

The package intentionally contains only:

- `CdiRuntime.exe`
- `SDL2.dll`
- `player.cfg.example`
- this runtime README and the SDL2 third-party notice

It contains no BIOS ROM, disc image, recompiler, oracle, emulator-derived
tooling, trace, or development utility. Supply your own compatible CD-i system
ROM and start the player from PowerShell:

```powershell
.\CdiRuntime.exe C:\path\to\cdi490a.rom
```

Player controls: arrows or WASD move the CD-i pointer; Enter, Space, or Z is
button 1; Backspace or X is button 2; F11 or Alt+Enter toggles fullscreen; Esc
exits. A standard controller maps D-pad/A/B. Dropping a valid CUE or BIN file
onto the window mounts media.

## Persistent options

On first normal launch, the runtime creates `player.cfg` in SDL's per-user
preference directory and prints its path. The two 0.0.1 enhancements remain
off by default:

```ini
[input]
capture_mouse = false

[rtc]
sync_host_on_startup = false
```

Mouse capture hides the Windows cursor only while the player has focus and
maps relative motion plus left/right clicks through the emulated CD-i input
device. Alt+Tab releases it. RTC synchronization samples host-local time once
before guest startup and never continuously corrects the guest clock.

Known scope: the recompiled CD-RTOS BIOS reaches and renders its player shell,
including real-time hardware input and media-state handling. CD-i application
loading and gameplay are not part of this checkpoint. The runtime will reject
or fail to launch incomplete/synthetic media as real CD-i software.

This private internal checkpoint is not a public redistribution or licensing
grant. SDL2 is redistributed under the notice in
`THIRD-PARTY-NOTICES.md`.
