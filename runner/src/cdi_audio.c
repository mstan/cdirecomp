#include "cdi_audio.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

enum {
    XA_PAYLOAD_OFFSET = 12,
    XA_SOUND_GROUPS = 18,
    XA_SOUND_GROUP_BYTES = 128,
    XA_SAMPLES_PER_UNIT = 28,
    AUDIO_RING_FRAMES = 1u << 17,
    AUDIO_RING_MASK = AUDIO_RING_FRAMES - 1u
};

typedef struct {
    int32_t previous[2][2];
    int16_t ring[AUDIO_RING_FRAMES][2];
    atomic_uint write_index;
    atomic_uint read_index;
    atomic_ullong sectors;
    atomic_ullong sample_frames;
    atomic_ullong pcm_hash;
    atomic_ullong dropped_frames;
} CdiAudio;

static CdiAudio audio;

static int16_t clamp_sample(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

static int16_t decode_sample(int32_t source, uint8_t parameter,
                             unsigned channel, int eight_bit) {
    static const int16_t filter[4][2] = {
        { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }
    };
    unsigned shift = parameter & 15u;
    unsigned index = parameter >> 4;
    int32_t decoded;

    if (shift > 12u) shift = 12u;
    if (index > 3u) index = 0u;
    decoded = source * (1 << (eight_bit ? 8 : 12));
    decoded >>= shift;
    decoded += (audio.previous[channel][0] * filter[index][0] +
                audio.previous[channel][1] * filter[index][1] + 32) >> 6;
    decoded = clamp_sample(decoded);
    audio.previous[channel][1] = audio.previous[channel][0];
    audio.previous[channel][0] = decoded;
    return (int16_t)decoded;
}

static void hash_sample(uint64_t *hash, int16_t sample) {
    const uint64_t prime = UINT64_C(0x100000001B3);
    uint16_t bits = (uint16_t)sample;
    *hash ^= (uint8_t)bits;
    *hash *= prime;
    *hash ^= (uint8_t)(bits >> 8);
    *hash *= prime;
}

static void emit_frame(int16_t left, int16_t right, int duplicate,
                       uint64_t *hash, uint64_t *frames, uint64_t *dropped) {
    unsigned copies = duplicate ? 2u : 1u;
    while (copies--) {
        uint32_t write = atomic_load_explicit(&audio.write_index,
                                               memory_order_relaxed);
        uint32_t read = atomic_load_explicit(&audio.read_index,
                                              memory_order_acquire);
        hash_sample(hash, left);
        hash_sample(hash, right);
        (*frames)++;
        if (write - read >= AUDIO_RING_FRAMES) {
            (*dropped)++;
            continue;
        }
        audio.ring[write & AUDIO_RING_MASK][0] = left;
        audio.ring[write & AUDIO_RING_MASK][1] = right;
        atomic_store_explicit(&audio.write_index, write + 1u,
                              memory_order_release);
    }
}

void cdi_audio_reset(void) {
    memset(&audio.previous, 0, sizeof audio.previous);
    atomic_store_explicit(&audio.write_index, 0, memory_order_relaxed);
    atomic_store_explicit(&audio.read_index, 0, memory_order_relaxed);
    atomic_store_explicit(&audio.sectors, 0, memory_order_relaxed);
    atomic_store_explicit(&audio.sample_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&audio.pcm_hash, UINT64_C(0xCBF29CE484222325),
                          memory_order_relaxed);
    atomic_store_explicit(&audio.dropped_frames, 0, memory_order_relaxed);
}

int cdi_audio_decode_sector(const uint8_t sector[2340]) {
    uint8_t coding;
    int stereo, eight_bit, duplicate;
    unsigned units;
    uint64_t hash, frames = 0, dropped = 0;

    if (!sector || sector[3] != 2u || !(sector[6] & 0x04u)) return 0;
    coding = sector[7];
    stereo = (coding & 3u) == 1u;
    eight_bit = (coding & 0x30u) == 0x10u;
    duplicate = (coding & 0x0Cu) == 0x04u; /* 18.9 kHz -> 37.8 kHz output. */
    units = eight_bit ? 4u : 8u;
    hash = atomic_load_explicit(&audio.pcm_hash, memory_order_relaxed);

    for (unsigned group_index = 0; group_index < XA_SOUND_GROUPS;
         group_index++) {
        const uint8_t *group = sector + XA_PAYLOAD_OFFSET +
                               group_index * XA_SOUND_GROUP_BYTES;
        int16_t decoded[8][XA_SAMPLES_PER_UNIT];

        for (unsigned unit = 0; unit < units; unit++) {
            unsigned channel = stereo ? (unit & 1u) : 0u;
            uint8_t parameter = group[4u + (unit & 3u) +
                                      ((unit & 4u) ? 8u : 0u)];
            for (unsigned sample = 0; sample < XA_SAMPLES_PER_UNIT; sample++) {
                uint8_t packed = group[16u + sample * 4u +
                                       (eight_bit ? unit : unit / 2u)];
                int32_t source;
                if (eight_bit)
                    source = (int8_t)packed;
                else {
                    unsigned nibble = (unit & 1u) ? packed >> 4 : packed & 15u;
                    source = (nibble & 8u) ? (int32_t)nibble - 16 : (int32_t)nibble;
                }
                decoded[unit][sample] =
                    decode_sample(source, parameter, channel, eight_bit);
            }
        }

        if (stereo) {
            for (unsigned unit = 0; unit < units; unit += 2u)
                for (unsigned sample = 0; sample < XA_SAMPLES_PER_UNIT; sample++)
                    emit_frame(decoded[unit][sample], decoded[unit + 1u][sample],
                               duplicate, &hash, &frames, &dropped);
        } else {
            for (unsigned unit = 0; unit < units; unit++)
                for (unsigned sample = 0; sample < XA_SAMPLES_PER_UNIT; sample++)
                    emit_frame(decoded[unit][sample], decoded[unit][sample],
                               duplicate, &hash, &frames, &dropped);
        }
    }

    atomic_fetch_add_explicit(&audio.sectors, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&audio.sample_frames, frames, memory_order_relaxed);
    atomic_store_explicit(&audio.pcm_hash, hash, memory_order_relaxed);
    atomic_fetch_add_explicit(&audio.dropped_frames, dropped,
                              memory_order_relaxed);
    return 1;
}

uint32_t cdi_audio_read_frames(int16_t *pcm, uint32_t capacity) {
    uint32_t read, write, available;
    if (!pcm || !capacity) return 0;
    read = atomic_load_explicit(&audio.read_index, memory_order_relaxed);
    write = atomic_load_explicit(&audio.write_index, memory_order_acquire);
    available = write - read;
    if (available > capacity) available = capacity;
    for (uint32_t i = 0; i < available; i++) {
        pcm[i * 2u] = audio.ring[(read + i) & AUDIO_RING_MASK][0];
        pcm[i * 2u + 1u] = audio.ring[(read + i) & AUDIO_RING_MASK][1];
    }
    atomic_store_explicit(&audio.read_index, read + available,
                          memory_order_release);
    return available;
}

void cdi_audio_debug_state(CdiAudioState *out) {
    uint32_t read, write;
    if (!out) return;
    read = atomic_load_explicit(&audio.read_index, memory_order_acquire);
    write = atomic_load_explicit(&audio.write_index, memory_order_acquire);
    out->sectors = atomic_load_explicit(&audio.sectors, memory_order_relaxed);
    out->sample_frames = atomic_load_explicit(&audio.sample_frames,
                                               memory_order_relaxed);
    out->pcm_fnv1a = atomic_load_explicit(&audio.pcm_hash, memory_order_relaxed);
    out->dropped_frames = atomic_load_explicit(&audio.dropped_frames,
                                                memory_order_relaxed);
    out->queued_frames = write - read;
}
