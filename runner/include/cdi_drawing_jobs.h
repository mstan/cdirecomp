#pragma once
#include "cdi_runtime.h"

extern RecompFuncPtr cdi_drawing_entry;
int cdi_drawing_dispatch(RecompFuncPtr function);
void cdi_drawing_reset(void);
void cdi_drawing_sync_cpu(void);
typedef struct {
    uint64_t jobs, empty_checks, callbacks, interrupted, fallbacks;
} CdiDrawingJobState;
void cdi_drawing_job_state(CdiDrawingJobState *out);
