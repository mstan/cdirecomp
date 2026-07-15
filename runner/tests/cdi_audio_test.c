#include "cdi_audio.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(test) do { \
    if (!(test)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); failures++; } \
} while (0)

static void make_audio_sector(uint8_t sector[2340], uint8_t coding,
                              uint8_t packed) {
    memset(sector, 0, 2340);
    sector[3] = 2;
    sector[6] = sector[10] = 0x64;
    sector[7] = sector[11] = coding;
    for (unsigned group = 0; group < 18; group++) {
        uint8_t *sound = sector + 12 + group * 128;
        memset(sound + 16, packed, 112);
    }
}

int main(void) {
    uint8_t sector[2340];
    int16_t pcm[4032 * 2];
    CdiAudioState state;

    cdi_audio_reset();
    make_audio_sector(sector, 0x01, 0x11); /* 37.8 kHz, 4-bit stereo. */
    CHECK(cdi_audio_decode_sector(sector) == 1);
    cdi_audio_debug_state(&state);
    CHECK(state.sectors == 1);
    CHECK(state.sample_frames == 2016);
    CHECK(state.queued_frames == 2016);
    CHECK(state.pcm_fnv1a != UINT64_C(0xCBF29CE484222325));
    CHECK(cdi_audio_read_frames(pcm, 4032) == 2016);
    CHECK(pcm[0] == 4096 && pcm[1] == 4096);

    cdi_audio_reset();
    make_audio_sector(sector, 0x05, 0x11); /* 18.9 kHz duplicates frames. */
    CHECK(cdi_audio_decode_sector(sector) == 1);
    cdi_audio_debug_state(&state);
    CHECK(state.sample_frames == 4032);
    CHECK(cdi_audio_read_frames(pcm, 4032) == 4032);
    CHECK(pcm[0] == pcm[2] && pcm[1] == pcm[3]);

    cdi_audio_reset();
    make_audio_sector(sector, 0x10, 0x01); /* 37.8 kHz, 8-bit mono. */
    CHECK(cdi_audio_decode_sector(sector) == 1);
    cdi_audio_debug_state(&state);
    CHECK(state.sample_frames == 2016);
    CHECK(cdi_audio_read_frames(pcm, 4032) == 2016);
    CHECK(pcm[0] == 256 && pcm[1] == 256);

    if (failures) return 1;
    puts("CD-i XA audio tests passed");
    return 0;
}
