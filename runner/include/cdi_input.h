#pragma once
#include <stdint.h>
/* Physical SDL keys/controller and developer playback are independent
 * producers. An idle frontend must not release another producer's button. */
void cdi_input_set_frontend(uint32_t mask);
