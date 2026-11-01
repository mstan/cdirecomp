# cdirecomp 0.0.3 Windows runtime

This research preview runs the real CD-RTOS BIOS and includes the qualified
native jobs and resident GPU renderer used by HotelMarioRecomp. Full campaigns,
bosses, ending, audio listening and correct active-stage save restoration remain
unverified. Minor visual flickering is a known open issue.

The ZIP contains CdiRuntime.exe, SDL2.dll, this README, a player configuration
example and third-party notices. Supply your own matching 512KiB cdi490a BIOS
and, for Hotel Mario, the USA Mode-2 CUE/BIN disc:

```powershell
.\CdiRuntime.exe C:\path\to\cdi490a.rom
.\CdiRuntime.exe C:\path\to\cdi490a.rom --disc "C:\path\to\Hotel Mario (USA).cue"
```

No BIOS, disc data, generated sources, oracle or development tools are included.
Other games and BIOS revisions are not qualified by this release.

Arrows/WASD move; Enter/Space/Z is button1; Backspace/X is button2. A controller
uses D-pad/A/B. F11 or Alt+Enter toggles fullscreen; Esc exits. Dropping a valid
CUE/BIN mounts media. F8 cycles internal density through 1/2/4/8/16/32/64x;
the initial density is 1x. Compatible OpenGL hardware renders owned raster jobs;
unsupported drivers retain exact native output. Density affects presentation
only. Required16x was qualified on the owner's RTX3080Ti;32x/64x full-speed
operation and lower-end16x remain unqualified, and64x misses native cadence.

Normal launches create player.cfg and nvram.bin in SDL's per-user preference
directory and print their paths. Preserve nvram.bin to keep native saves.
Mouse capture and startup host-clock synchronization are off by default:

```ini
[input]
capture_mouse = false
[rtc]
sync_host_on_startup = false
```

Mouse capture releases on focus loss. RTC sync samples the host clock once at
startup. The runtime currently has no save-state/rollback feature. Native
battery saves are a separate contract.

The Windows runtime package explicitly enables the qualified native-job build
selections. Source builds retain independent LLE/HLE choices; generic engine
source defaults remain LLE. See the repository's HLE burndown/evidence for exact
workload scopes, raw results and limitations. SDL2 notices are in
THIRD-PARTY-NOTICES.md; repository licensing applies.
