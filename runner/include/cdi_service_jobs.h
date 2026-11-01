#pragma once
#include "cdi_runtime.h"

extern RecompFuncPtr cdi_service_entry;
int cdi_service_dispatch(RecompFuncPtr function);
void cdi_service_reset(void);
void cdi_service_sync_cpu(void);
typedef struct {
    uint64_t jobs, errors, interrupted, fallbacks;
} CdiServiceJobState;
void cdi_service_job_state(CdiServiceJobState *out);
