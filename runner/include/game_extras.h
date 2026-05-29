/*
 * game_extras.h — per-game hooks included by the generated TUs.
 *
 * The shared 68000 frontend emits `#include "game_extras.h"` into every
 * generated file (inherited from the Genesis frontend). It is the seam for
 * hand-written, game-specific glue (entry points, data-section symbols, custom
 * dispatch helpers). Hotel Mario needs nothing here yet; keep it minimal so
 * generated code compiles.
 */
#pragma once
#include "cdi_runtime.h"

/* Per-game/OS dispatch override, called by the generated dispatch when a target
 * address isn't in the recompiled table. Return nonzero if hand-written code
 * handled it; 0 lets the runtime log a dispatch miss. Default impl (runner)
 * returns 0. */
int game_dispatch_override(uint32_t addr);
