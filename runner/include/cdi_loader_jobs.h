#pragma once
#include "cdi_runtime.h"

/* Build-selected native loader work. A zero return retains faithful dispatch. */
int cdi_loader_dispatch(RecompFuncPtr function);
extern RecompFuncPtr cdi_loader_entry;
/* Materialize an active job before an asynchronous debugger snapshot. */
void cdi_loader_sync_cpu(void);
void cdi_loader_reset(void);
typedef struct {
    /* yielded counts native handoffs to the real kernel's scheduling check,
     * including checks that decide no process switch is necessary. */
    uint64_t spans, words, interrupted, yielded;
} CdiLoaderJobState;
void cdi_loader_job_state(CdiLoaderJobState *out);
