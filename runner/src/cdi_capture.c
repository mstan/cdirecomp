#include "cdi_capture.h"
#include "cdi_audio.h"
#include "cdi_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define CDI_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define CDI_MKDIR(p) mkdir(p, 0777)
#endif

#define CAPTURE_FPS 59.94

static int s_requested, s_active, s_done;
static char s_out[1024], s_wav_path[1024];
static uint64_t s_from, s_to = UINT64_MAX;
static FILE *s_wav;
static uint64_t s_wav_frames;
static double s_audio_due;          /* fractional output frames owed */
static int16_t *s_queue;            /* decoded but not yet emitted */
static size_t s_queue_n, s_queue_cap;
static uint64_t s_written;

int cdi_capture_requested(void) { return s_requested; }

int cdi_capture_parse_arg(int argc, char **argv, int *i)
{
    const char *a = argv[*i];
    if (!strcmp(a, "--capture-out") && *i + 1 < argc) {
        snprintf(s_out, sizeof s_out, "%s", argv[++*i]);
    } else if (!strcmp(a, "--capture-wav") && *i + 1 < argc) {
        snprintf(s_wav_path, sizeof s_wav_path, "%s", argv[++*i]);
    } else if (!strcmp(a, "--capture-from") && *i + 1 < argc) {
        s_from = strtoull(argv[++*i], NULL, 0);
    } else if (!strcmp(a, "--capture-to") && *i + 1 < argc) {
        s_to = strtoull(argv[++*i], NULL, 0);
    } else {
        return 0;
    }
    s_requested = 1;
    return 1;
}

static void output_error(const char *operation)
{
    fprintf(stderr, "cdi capture: %s failed\n", operation);
    exit(2);
}

static void write_bytes(FILE *f, const void *bytes, size_t count)
{
    if (count && fwrite(bytes, 1, count, f) != count) output_error("output write");
}

static void put32(FILE *f, uint32_t v)
{
    const uint8_t bytes[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    write_bytes(f, bytes, sizeof bytes);
}

static void put16(FILE *f, uint16_t v)
{
    const uint8_t bytes[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    write_bytes(f, bytes, sizeof bytes);
}

static void wav_header(FILE *f, uint64_t frames)
{
    uint32_t bytes;
    if (frames > (UINT32_MAX - 36u) / 4u) output_error("WAV size limit");
    bytes = (uint32_t)(frames * 4u);
    write_bytes(f, "RIFF", 4); put32(f, 36 + bytes); write_bytes(f, "WAVEfmt ", 8);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, CDI_AUDIO_OUTPUT_RATE);
    put32(f, CDI_AUDIO_OUTPUT_RATE * 4u); put16(f, 4); put16(f, 16);
    write_bytes(f, "data", 4); put32(f, bytes);
}

static void queue_drain(void)
{
    int16_t tmp[4096 * 2];
    uint32_t n;
    while ((n = cdi_audio_read_frames(tmp, 4096)) != 0) {
        if (s_queue_n + n * 2u > s_queue_cap) {
            s_queue_cap = (s_queue_n + n * 2u) * 2u;
            s_queue = (int16_t *)realloc(s_queue, s_queue_cap * sizeof *s_queue);
            if (!s_queue) { fprintf(stderr, "cdi capture: out of memory\n"); exit(2); }
        }
        memcpy(s_queue + s_queue_n, tmp, (size_t)n * 2u * sizeof *tmp);
        s_queue_n += (size_t)n * 2u;
    }
}

/* Emit exactly one field's worth of output frames: queued PCM first, silence
 * when the guest is not playing anything. */
static void audio_field(int record)
{
    uint32_t want, take, i;
    queue_drain();
    s_audio_due += CDI_AUDIO_OUTPUT_RATE / CAPTURE_FPS;
    want = (uint32_t)s_audio_due;
    s_audio_due -= want;
    take = (uint32_t)(s_queue_n / 2u);
    if (take > want) take = want;
    if (record && s_wav) {
        if (s_wav_frames > (UINT32_MAX - 36u) / 4u - want) output_error("WAV size limit");
        write_bytes(s_wav, s_queue, (size_t)take * 2u * sizeof *s_queue);
        for (i = take; i < want; i++) { put16(s_wav, 0); put16(s_wav, 0); }
        s_wav_frames += want;
    }
    /* Keep the PCM queue aligned to every guest field, including fields before
     * --capture-from and image-only captures. They must not replay old audio. */
    if (take) {
        memmove(s_queue, s_queue + (size_t)take * 2u, (s_queue_n - (size_t)take * 2u) * sizeof *s_queue);
        s_queue_n -= (size_t)take * 2u;
    }
}

static void on_field(uint64_t frame, const uint32_t *argb, int w, int h)
{
    const int record = frame >= s_from && frame <= s_to;
    if (s_done) return;
    audio_field(record);
    if (record && s_out[0]) {
        char path[1100];
        FILE *f;
        snprintf(path, sizeof path, "%s/frame_%06llu.ppm", s_out, (unsigned long long)frame);
        f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "cdi capture: cannot write %s\n", path); exit(2); }
        if (fprintf(f, "P6\n%d %d\n255\n", w, h) < 0) output_error("frame header write");
        {
            uint8_t *row = (uint8_t *)malloc((size_t)w * 3u);
            int x, y;
            if (!row) output_error("frame allocation");
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    uint32_t p = argb[(size_t)y * (size_t)w + (size_t)x];
                    row[x * 3] = (uint8_t)(p >> 16); row[x * 3 + 1] = (uint8_t)(p >> 8); row[x * 3 + 2] = (uint8_t)p;
                }
                write_bytes(f, row, (size_t)w * 3u);
            }
            free(row);
        }
        if (fclose(f) != 0) output_error("frame close");
        s_written++;
    }
    if (frame >= s_to) { cdi_capture_finish(); exit(0); }
}

void cdi_capture_begin(void)
{
    if (!s_requested) return;
    if (s_out[0]) CDI_MKDIR(s_out);
    if (s_wav_path[0]) {
        s_wav = fopen(s_wav_path, "wb");
        if (!s_wav) { fprintf(stderr, "cdi capture: cannot write %s\n", s_wav_path); exit(2); }
        wav_header(s_wav, 0);
        if (fflush(s_wav) != 0) output_error("WAV header flush");
    }
    g_mcd212_field_hook = on_field;
    s_active = 1;
    fprintf(stderr, "cdi capture: out=%s from=%llu to=%llu wav=%s\n", s_out[0] ? s_out : "(none)",
            (unsigned long long)s_from, (unsigned long long)s_to, s_wav_path[0] ? s_wav_path : "(none)");
}

void cdi_capture_finish(void)
{
    if (!s_active || s_done) return;
    s_done = 1;
    if (s_wav) {
        if (fflush(s_wav) != 0) output_error("WAV data flush");
        if (fseek(s_wav, 0, SEEK_SET) != 0) output_error("WAV header seek");
        wav_header(s_wav, s_wav_frames);
        if (fclose(s_wav) != 0) output_error("WAV close");
        s_wav = NULL;
    }
    g_mcd212_field_hook = NULL;
    free(s_queue);
    s_queue = NULL;
    s_queue_n = s_queue_cap = 0;
    fprintf(stderr, "cdi capture: done fields_written=%llu audio_frames=%llu\n",
            (unsigned long long)s_written, (unsigned long long)s_wav_frames);
}
