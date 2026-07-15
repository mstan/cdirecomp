#pragma once

#include "cdi_runtime.h"

/* Sample host-local civil time once. This function has no persistent clock
 * relationship with the emulated DS1216. */
int cdi_host_local_time(CdiRtcTime *time_value);
