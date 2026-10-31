#pragma once
#include "cdi_runtime.h"

extern RecompFuncPtr cdi_memory_entry;
int cdi_memory_dispatch(RecompFuncPtr function);
void cdi_memory_reset(void);
typedef struct {
    uint64_t jobs, bytes, rows, interrupted, fallbacks;
} CdiMemoryJobState;
void cdi_memory_job_state(CdiMemoryJobState *out);
