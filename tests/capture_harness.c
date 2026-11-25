#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_write, fail_seek, fail_flush, fail_close;

static size_t capture_fwrite(const void *p, size_t size, size_t count, FILE *f)
{
    return fail_write ? 0 : fwrite(p, size, count, f);
}
static int capture_fseek(FILE *f, long offset, int origin)
{
    return fail_seek ? -1 : fseek(f, offset, origin);
}
static int capture_fflush(FILE *f)
{
    return fail_flush ? EOF : fflush(f);
}
static int capture_fclose(FILE *f)
{
    int result = fclose(f);
    return fail_close ? EOF : result;
}

/* Exercise the production recorder with deterministic device PCM and injected
 * stdio failures. No guest CPU, firmware or disc is part of this fixture. */
#define fwrite capture_fwrite
#define fseek capture_fseek
#define fflush capture_fflush
#define fclose capture_fclose
#ifndef CDI_CAPTURE_SOURCE
#define CDI_CAPTURE_SOURCE "../runner/src/cdi_capture.c"
#endif
#include CDI_CAPTURE_SOURCE
#undef fwrite
#undef fseek
#undef fflush
#undef fclose

void (*g_mcd212_field_hook)(uint64_t, const uint32_t *, int, int);
static unsigned pending_frames;
static int16_t field_value;

uint32_t cdi_audio_read_frames(int16_t *stereo, uint32_t capacity)
{
    unsigned n = pending_frames < capacity ? pending_frames : capacity;
    unsigned i;
    for (i = 0; i < n; ++i) {
        stereo[i * 2] = field_value;
        stereo[i * 2 + 1] = (int16_t)-field_value;
    }
    pending_frames -= n;
    return n;
}

static void option(const char *name, const char *value)
{
    char *args[2] = {(char *)name, (char *)value};
    int index = 0;
    if (!cdi_capture_parse_arg(2, args, &index) || index != 1) abort();
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "range";
    unsigned frame, total = !strcmp(mode, "image-only") ? 200 : 4;
    uint32_t pixel = 0xff123456;
    int images_only = !strcmp(mode, "image-only") || !strcmp(mode, "close-frame");
    if (!images_only) option("--capture-wav", !strcmp(mode, "bad-wav") ? "absent/audio.wav" :
                             !strcmp(mode, "full-device") ? "/dev/full" : "audio.wav");
    if (!strcmp(mode, "range") || images_only) option("--capture-out", "frames");
    if (!strcmp(mode, "bad-frame")) option("--capture-out", "absent/frames");
    if (!strcmp(mode, "range")) option("--capture-from", "3");
    cdi_capture_begin();
    if (!strcmp(mode, "short-write")) fail_write = 1;
    if (!strcmp(mode, "close-frame")) fail_close = 1;
    for (frame = 1; frame <= total; ++frame) {
        /* The hardware mix produces precisely this field's worth of samples. */
        pending_frames = (unsigned)(37800.0 * frame / 59.94) -
                         (unsigned)(37800.0 * (frame - 1) / 59.94);
        field_value = (int16_t)frame;
        if (!strcmp(mode, "silence")) pending_frames = 0;
        g_mcd212_field_hook(frame, &pixel, 1, 1);
    }
    if (!strcmp(mode, "seek")) fail_seek = 1;
    if (!strcmp(mode, "flush")) fail_flush = 1;
    if (!strcmp(mode, "close-wav")) fail_close = 1;
    cdi_capture_finish();
    return 0;
}
