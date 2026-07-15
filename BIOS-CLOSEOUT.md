# BIOS/player-shell closeout

**Closed:** 2026-07-14
**Scope:** the non-launching CD-RTOS player shell on the user-supplied CDI 490
system ROM. Application loading and gameplay are explicitly outside this
milestone.

## Acceptance result

The BIOS/player-shell chapter is complete. The recompiled ROM now:

- boots natively to the canonical persistent shell `STOP` at post-instruction
  `PC=$40A3E6`, `SR=$2000`, with zero RULE 0a dispatch misses;
- renders the MCD212 shell, paces it in real time, and services the real guest
  interrupt paths;
- navigates by keyboard, controller, and true relative host-mouse input through
  the timed IKAT channel-A driver rather than direct cursor or RAM writes;
- mounts and ejects valid Mode-2 media through the enabled IKAT channel-D path
  without launching the fixture;
- traverses the real **Options**, empty **Storage**, and **Exit** UI actions,
  returning to the persistent shell after each action with no dispatch miss;
- models the DS1216 RTC and preserves its 32 KiB battery-backed SRAM between
  normal player launches, eliminating the cold-battery system-configuration
  warning after the BIOS initializes it; and
- optionally seeds the RTC clock from host-local time once at startup while
  leaving subsequent guest clock writes authoritative.

`tools/bios_options_smoke.py` is the closeout regression. It creates a fresh
battery image, lets the real BIOS initialize it, reboots from that image, moves
the CD-i pointer through relative IKAT reports, clicks Options → Storage →
Options → Exit, verifies guest-driver drains and framebuffer changes, and
confirms deterministic/headless execution neither loads nor rewrites player
NVRAM.

## Persistence contract

A normal player profile stores `nvram.bin` beside `player.cfg`. The file is an
exact 32 KiB image of DS1216 SRAM and is replaced atomically on clean shutdown.
The live RTC clock is deliberately not serialized: it retains the deterministic
1989 seed or the optional one-shot host seed independently of SRAM persistence.
Malformed battery images fail clearly instead of partially mutating emulated
memory. Headless, fixed-sequence, scripted-input, and co-sim profiles do not
read, create, or rewrite player NVRAM.

## Evidence retained

- Debug and Release runner tests: 6/6.
- Co-sim self-tests: 10/10; all four 20,000-record trust gates pass.
- Shell idle/pacing, media insert/eject, BIOS navigation, RTC startup, and BIOS
  Options/NVRAM closeout smokes: all pass.
- Options/Storage/Exit traversal: ordered press/release packets, real `pt1driv`
  guest drains, visible framebuffer transitions, shell return, zero misses.
- Existing near-full-boot cycle audit: 659,998 transitions with zero resyncs,
  cost mismatches, or cycle drift.

## What moves to the next chapter

The closeout does not claim that every SCC68070 facility or every possible ROM
path has executed. I2C, DMA, MMU translation, additional exception variants,
CIAP application-sector delivery, audio, and dynamically loaded OS-9 code stay
on the platform/game backlog. They are no longer speculative blockers for a
BIOS shell that does not use them; the Hotel Mario loader path will drive their
implementation and add focused regressions when it reaches them.

The next critical path is MC-CDI-024: connect relocated OS-9 modules loaded by
the real recompiled CD-RTOS to their statically recompiled native functions.
