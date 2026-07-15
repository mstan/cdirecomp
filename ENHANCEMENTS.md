# ENHANCEMENTS.md — opt-in deviations from faithful CD-i behavior

cdirecomp's default is a **byte-faithful** CD-i: same OS, same device behavior,
same output as CeDImu/real hardware. Enhancements (widescreen, higher internal
resolution, faster load skips, input remaps) are allowed only under the rules
below, which the sibling projects converged on. They exist so faithfulness and
quality-of-life never get confused for one another.

## Rule 1 — Default is authentic, always

Every enhancement is **opt-in**, **off by default**, and **byte-identical to the
faithful path when off**. A standard build with all toggles off must produce the
exact frames, audio, and RAM state CeDImu produces. If turning a feature off
isn't byte-identical, it's a bug, not an enhancement.

## Rule 2 — Split runner *capability* from game *policy*

The runner provides a generic, parameterised, inert-by-default capability (e.g.
"render N extra columns of MCD212 plane"). The per-game/per-OS config supplies
title-specific policy. Host preferences such as mouse capture and clock seeding
belong instead to persistent launcher/player configuration, never `game.cfg`.
The runner never hardcodes a title. (Mirrors segagenesisrecomp's `g_game_spec` /
`g_game_layout` split.)

## Rule 3 — Never hand-edit generated C

Generated `*.c` is rebuilt from the recompiler and must never be edited (the
project's first law). Enhancements that need to touch recompiled code use a
**post-regen, build-time injector**: anchored-block patches or whole-function
override prologues that are idempotent and reversible, applied after emission.
Same as the recompiler discipline for everything else.

## Rule 4 — Gate on state; keep recordings vanilla

Enhancements gate on live OS/game state, not wall-clock. Anything that must stay
canonical for correctness (a recorded boot trace, a regression smoke snapshot,
an oracle diff) runs with enhancements OFF so it stays comparable.

## Rule 5 — Respect hardware representational limits

DYUV/CLUT/RGB555 palettes, plane sizes, and audio rates are real constraints.
An enhancement that widens a buffer must clamp in every consumer; never assume
a wider intermediate silently survives a faithful stage.

## Rule 6 — Ship both asset sets

When an enhancement changes emitted assets, ship the pristine faithful set and
the enhanced set side by side. The faithful set is what the oracle diff uses.

## Rule 7 — Same debugging discipline

Enhancement bugs are debugged exactly like faithful bugs: ring buffers, first
divergence vs the faithful path, fix the class. No printf, no eyeballing.

---

## Implemented player-quality enhancements

The BIOS player shell is bootable, rendered, real-time paced, and physically
controllable. These first host-integration enhancements are implemented through
the durable per-user `player.cfg` contract. Enabled and disabled values pass
load/save round trips. A deterministic profile suppresses them for that run but
does not read, create, or overwrite the user's saved choices. The repository
does not yet contain a graphical launcher; its eventual UI uses this same
player-config file/API rather than inventing CLI-only state.

### Captured host mouse (MC-CDI-027)

The opt-in persistent `input.capture_mouse` player preference uses the host
mouse as the CD-i relative pointing device while the SDL window has input
focus. It is saved in the user's player config so it survives future launches;
it is not a command-line-only or per-title option.

- On focus gain, hide the host cursor and enable SDL relative-mouse capture. On
  focus loss, shutdown, or capture disable, release capture and restore the host
  cursor immediately. Never trap a pointer in an unfocused window.
- Feed accumulated relative X/Y motion through a transport-neutral runtime ABI
  and the existing emulated-time, 25-ms IKAT packet path. Do not write the BIOS
  cursor position or guest RAM directly, and do not reduce mouse motion to the
  current keyboard acceleration steps.
- Map the primary host button to CD-i button 1 and the secondary host button to
  CD-i button 2; press and release transitions must both reach the guest.
- Preserve representable motion across IKAT packets (including clamping and
  carrying any remainder) so fast host movement is not silently discarded.
- With the option off, retain the current visible host cursor and keyboard/
  controller behavior byte-for-byte. Headless, scripted-input, co-sim, and
  regression runs keep capture off.

Acceptance is met by `CdiInputTest`, `PlayerConfigTest`, the press/release
assertions in `tools/shell_idle_smoke.py`, and the end-to-end relative-motion
path in `tools/bios_navigation_smoke.py` (exact delta, enabled IRQ, guest drain,
visible cursor/framebuffer movement, STOP return, and RULE 0a cleanliness).

### One-shot host clock seed (MC-CDI-028)

The opt-in persistent `rtc.sync_host_on_startup` player preference samples the
host's local civil date/time once per emulated startup and uses it to seed the
CD-i RTC before the first guest instruction. The player config persists the
preference; enabling it does not imply continuous synchronization.

- Synchronize only the DS1216 clock fields; preserve the faithful NVRAM reset/
  persistence policy independently.
- After startup, advance solely from emulated SCC68070 cycles. Never poll or
  continuously correct against wall-clock time, so guest changes to the date,
  time, oscillator state, or hundredths remain authoritative at runtime.
- Use the portable host-local time APIs on Windows, macOS, and Linux, validate
  the value against the RTC's representable range, and fail clearly rather than
  wrap an invalid date.
- With the option off, keep the deterministic hardware-model
  `1989-01-01 00:00:00` seed exactly. Co-sim, recorded traces, baseline smokes,
  and oracle comparisons keep synchronization off.

Acceptance is met by `CdiNvramTest` (faithful seed, valid/invalid host seed,
cycle rollover, SRAM independence, and no re-seed after a guest RTC write),
`PlayerConfigTest`, and `tools/rtc_startup_smoke.py`, which boots a real BIOS
with a temporary enabled persistent config and verifies one host-local sample,
the canonical shell STOP, and an unchanged config file.
