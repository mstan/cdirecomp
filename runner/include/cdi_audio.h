#pragma once

#include <stdint.h>

enum {
    CDI_AUDIO_OUTPUT_RATE = 37800,
    CDI_AUDIO_OUTPUT_CHANNELS = 2
};

typedef struct CdiAudioState {
    uint64_t sectors;
    uint64_t sample_frames;
    uint64_t pcm_fnv1a;
    uint64_t dropped_frames;
    uint32_t queued_frames;
} CdiAudioState;

/* Deterministic CD-i/XA ADPCM decoder. The core always decodes and hashes
 * samples, including in headless runs; the SDL frontend merely drains the
 * optional PCM queue. */
void cdi_audio_reset(void);
int  cdi_audio_decode_sector(const uint8_t sector_body[2340]);
/* Decode the 18 sound groups in a CIAP ADPCM buffer. Returns output frames
 * at CDI_AUDIO_OUTPUT_RATE; the device uses that count for guest timing. */
uint32_t cdi_audio_decode_groups(const uint8_t sound_groups[2304], uint8_t coding);
uint32_t cdi_audio_read_frames(int16_t *stereo_pcm, uint32_t capacity_frames);
void cdi_audio_debug_state(CdiAudioState *out);
