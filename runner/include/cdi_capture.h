#pragma once
/* Unattended presented-field and audio capture (--capture-out and friends).
 * Each completed video field is written as frame_NNNNNN.ppm at its native
 * size; the CDIC/XA PCM is written to a 37.8 kHz stereo WAV aligned to guest
 * field time (silence where nothing plays). The run is unpaced and never opens
 * the SDL frontend, audio device or physical input; --input-script drives it. */
#include <stdint.h>

/* Consumes argv[*i] (and its value) when it is a capture option. */
int cdi_capture_parse_arg(int argc, char **argv, int *i);
int cdi_capture_requested(void);
/* Installs the field hook; call once the runtime is initialised. */
void cdi_capture_begin(void);
void cdi_capture_finish(void);
