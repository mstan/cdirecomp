/*
 * game_extras.h — per-game hooks included by the generated TUs.
 *
 * The shared 68000 frontend emits `#include "game_extras.h"` into every
 * generated file (inherited from the Genesis frontend). It is the seam for
 * hand-written, game-specific glue (entry points, data-section symbols, custom
 * dispatch helpers). Build-selected native jobs use the optional dispatch
 * seam; disabled jobs leave faithful generated execution intact.
 */
#pragma once
#include "cdi_runtime.h"

#if CDI_LOADER_HLE
#include "cdi_loader_jobs.h"
#define CDI_LOADER_JOB(fn) ((fn) == cdi_loader_entry && cdi_loader_dispatch(fn))
#else
#define CDI_LOADER_JOB(fn) 0
#endif
#if CDI_DRAWING_HLE
#include "cdi_drawing_jobs.h"
#define CDI_DRAWING_JOB(fn) ((fn) == cdi_drawing_entry && cdi_drawing_dispatch(fn))
#else
#define CDI_DRAWING_JOB(fn) 0
#endif
#if CDI_LOADER_HLE || CDI_DRAWING_HLE
#define RECOMP_NATIVE_JOB_DISPATCH(fn) (CDI_LOADER_JOB(fn) || CDI_DRAWING_JOB(fn))
#endif

/* Per-game/OS dispatch override, called by the generated dispatch when a target
 * address isn't in the recompiled table. Return nonzero if hand-written code
 * handled it; 0 lets the runtime log a dispatch miss. Default impl (runner)
 * returns 0. */
int game_dispatch_override(uint32_t addr);
