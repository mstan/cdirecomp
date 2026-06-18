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
the policy that turns it on for specific screens. The runner never hardcodes a
title. (Mirrors segagenesisrecomp's `g_game_spec` / `g_game_layout` split.)

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

No enhancements are implemented yet — cdirecomp is pre-boot-shell. This file is
the contract for when they are, so the first one lands the right way.
