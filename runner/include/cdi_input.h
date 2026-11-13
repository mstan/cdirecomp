#pragma once
#include <stdint.h>
/* Physical SDL keys/controller and developer playback are independent
 * producers. An idle frontend must not release another producer's button. */
void cdi_input_set_frontend(uint32_t mask);
void cdi_input_mouse_button_mask(uint32_t mask, int pressed);
