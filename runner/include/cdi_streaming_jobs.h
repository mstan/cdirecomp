#pragma once
#include <stdint.h>
typedef struct {
    uint64_t jobs, deferred_quanta, materializations;
} CdiStreamingJobState;
void cdi_streaming_job_state(CdiStreamingJobState *out);
/* CPU-thread publication before a held snapshot; live debug reads stay inert. */
void cdi_streaming_sync(void);
