/* Compare complete scanout contracts, including source consumption, hidden
 * plane/verdict diagnostics, cursor and undrawn/interlaced field retention.
 * Randomized legal controls supplement the hardware-derived pixel fixtures. */
#include "mcd212_video.h"
#include "cdi_runtime.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lle_mcd212_video_reset(void);
void lle_mcd212_video_control(int, uint32_t);
void lle_mcd212_video_begin_frame(uint16_t, uint16_t, int);
void lle_mcd212_video_draw_line(uint32_t, uint32_t, uint16_t, uint16_t,
                                 uint16_t, uint16_t *, uint16_t *);
void lle_mcd212_video_end_frame(void);
uint32_t lle_mcd212_video_copy_frame(uint32_t *, uint32_t, uint16_t *,
                                      uint16_t *, uint64_t *);
uint64_t lle_mcd212_video_frame_hash(uint16_t *, uint16_t *, uint64_t *);
uint32_t lle_mcd212_video_copy_changed_frame(uint32_t *, uint32_t, uint64_t,
    uint16_t *, uint16_t *, uint64_t *, uint64_t *);
void lle_mcd212_video_debug_state(Mcd212VideoDebugState *);
void lle_mcd212_video_debug_cursor(Mcd212VideoCursorState *);
unsigned lle_mcd212_video_debug_plane_row(int, unsigned, uint32_t *, unsigned);
unsigned lle_mcd212_video_debug_verdict_row(unsigned, uint8_t *, unsigned);

uint8_t g_ram0[CDI_RAM0_SIZE], g_ram1[CDI_RAM1_SIZE];
static uint32_t pixels[2][MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT];
static uint32_t rows[2][MCD212_VIDEO_MAX_WIDTH];
static uint32_t delivered[2][MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT];
static uint64_t content[2];
static uint8_t verdict[2][MCD212_VIDEO_MAX_WIDTH];
static uint32_t random_state = 0x21268070u;
static unsigned case_id, line_id, jobs, fused;
static uint64_t previous_fused;

static uint32_t random_word(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "scanout mismatch case=%u line=%u seed=$%08X: %s\n", \
            case_id, line_id, random_state, #test); exit(1); } } while (0)

static void control(int plane, uint32_t word) {
    mcd212_video_control(plane, word);
    lle_mcd212_video_control(plane, word);
}

static void compare_line(uint32_t a, uint32_t b, uint16_t wa, uint16_t wb) {
    uint16_t used[2][2];
    Mcd212VideoDebugState state[2];
    mcd212_video_draw_line(a, b, (uint16_t)line_id, wa, wb, &used[0][0], &used[0][1]);
    lle_mcd212_video_draw_line(a, b, (uint16_t)line_id, wa, wb, &used[1][0], &used[1][1]);
    CHECK(!memcmp(used[0], used[1], sizeof used[0]));
    mcd212_video_debug_state(&state[0]);
    lle_mcd212_video_debug_state(&state[1]);
    fused += (unsigned)(state[0].fused_jobs - previous_fused);
    previous_fused = state[0].fused_jobs;
    state[0].fused_jobs = state[1].fused_jobs = 0;
    CHECK(!memcmp(&state[0], &state[1], sizeof state[0]));
    for (int plane = 0; plane < 2; plane++) {
        unsigned width = mcd212_video_debug_plane_row(plane, line_id, rows[0], MCD212_VIDEO_MAX_WIDTH);
        CHECK(width == lle_mcd212_video_debug_plane_row(plane, line_id, rows[1], MCD212_VIDEO_MAX_WIDTH));
        CHECK(!memcmp(rows[0], rows[1], width * sizeof(uint32_t)));
    }
    unsigned width = mcd212_video_debug_verdict_row(line_id, verdict[0], MCD212_VIDEO_MAX_WIDTH);
    CHECK(width == lle_mcd212_video_debug_verdict_row(line_id, verdict[1], MCD212_VIDEO_MAX_WIDTH));
    CHECK(!memcmp(verdict[0], verdict[1], width));
    jobs++;
}

static void compare_frame(void) {
    uint16_t w[2], h[2];
    uint64_t gen[2];
    Mcd212VideoCursorState cursor[2] = {{0}};
    mcd212_video_end_frame();
    lle_mcd212_video_end_frame();
    uint32_t count = mcd212_video_copy_frame(pixels[0], sizeof pixels[0] / 4, &w[0], &h[0], &gen[0]);
    CHECK(count == lle_mcd212_video_copy_frame(pixels[1], sizeof pixels[1] / 4, &w[1], &h[1], &gen[1]));
    CHECK(w[0] == w[1] && h[0] == h[1] && gen[0] == gen[1]);
    CHECK(!memcmp(pixels[0], pixels[1], count * sizeof(uint32_t)));
    CHECK(mcd212_video_frame_hash(NULL, NULL, NULL) == lle_mcd212_video_frame_hash(NULL, NULL, NULL));
    uint64_t next_content[2];
    uint32_t sent = mcd212_video_copy_changed_frame(delivered[0], sizeof delivered[0] / 4,
        content[0], &w[0], &h[0], &gen[0], &next_content[0]);
    CHECK(sent == 0 || sent == count);
    CHECK(lle_mcd212_video_copy_changed_frame(delivered[1], sizeof delivered[1] / 4,
        content[1], &w[1], &h[1], &gen[1], &next_content[1]) == count);
    CHECK(w[0] == w[1] && h[0] == h[1] && gen[0] == gen[1]);
    CHECK(next_content[0] >= content[0] && next_content[1] > content[1]);
    CHECK(!memcmp(delivered[0], delivered[1], count * sizeof(uint32_t)));
    memcpy(content, next_content, sizeof content);
    mcd212_video_debug_cursor(&cursor[0]);
    lle_mcd212_video_debug_cursor(&cursor[1]);
    CHECK(!memcmp(&cursor[0], &cursor[1], sizeof cursor[0]));
}

int main(void) {
    const unsigned coding_a[] = {0, 1, 3, 4, 5, 11};
    const unsigned coding_b[] = {0, 1, 3, 5, 11};
    const unsigned widths[] = {1, 3, 160, 180, 360, 384};
    for (case_id = 0; case_id < 1024; case_id++) {
        unsigned base = widths[case_id % 6];
        unsigned interlaced = case_id & 1;
        mcd212_video_reset();
        lle_mcd212_video_reset();
        previous_fused = 0;
        memset(content, 0, sizeof content);
        if (!case_id) {
            CHECK(mcd212_video_frame_hash(NULL, NULL, NULL) ==
                  lle_mcd212_video_frame_hash(NULL, NULL, NULL));
            CHECK(!mcd212_video_copy_changed_frame(NULL, 0, 0, NULL, NULL, NULL, NULL));
        }
        for (unsigned bank = 0; bank < 4; bank++) {
            control(0, 0xC3000000u | bank);
            for (unsigned i = 0; i < 64; i++)
                control(0, ((0x80u + i) << 24) | (random_word() & 0xFFFFFFu));
        }
        control(0, 0xC0000000u | coding_a[case_id % 6] |
                   (coding_b[(case_id / 6) % 5] << 8) |
                   (random_word() & 0x004C0000u));
        control(0, 0xC1000000u | (random_word() & 0x00800F0Fu));
        control(0, 0xC2000000u | (random_word() & 1));
        control(0, 0xD8000000u | (random_word() & 15));
        for (unsigned plane = 0; plane < 2; plane++) {
            unsigned type = (case_id / (plane ? 24 : 6)) % 4;
            unsigned bpp = (case_id / (plane ? 8 : 4)) % 3;
            control(plane, 0x78000000u | type | (bpp << 8) | ((random_word() & 3) << 2));
            control(plane, (plane ? 0xDC000000u : 0xDB000000u) | (random_word() & 63));
            control(plane, (plane ? 0xC6000000u : 0xC4000000u) | (random_word() & 0xFFFFFFu));
            control(plane, (plane ? 0xC9000000u : 0xC7000000u) | (random_word() & 0xFFFFFFu));
            control(plane, (plane ? 0xCB000000u : 0xCA000000u) | (random_word() & 0xFFFFFFu));
            control(plane, (plane ? 0xDA000000u : 0xD9000000u) |
                           (case_id % 5 ? 0 : 0x00800000u) | (1 + random_word() % 17));
        }
        for (unsigned i = 0; i < 8; i++) {
            /* Unsorted/coincident edges and terminators exercise the ordered
             * control-list behavior, rather than assuming sorted mattes. */
            unsigned op = random_word() % 16;
            unsigned x = random_word() % (base * 2 + 16);
            control(0, ((0x50u + i) << 24) | (op << 20) |
                       ((random_word() & 1) << 16) |
                       ((random_word() & 63) << 10) | x);
        }
        control(0, 0xCD000000u | (2 << 12) | (random_word() % (base * 2)));
        control(0, 0xCE880000u | (random_word() & 0x0047800Fu));
        for (unsigned row = 0; row < 16; row++)
            control(0, 0xCF000000u | (row << 16) | (random_word() & 0xFFFF));
        for (unsigned field = 0; field < 3; field++) {
            /* Geometry and control changes between fields must invalidate
             * assumptions. Unwritten lines retain their previous contents. */
            unsigned field_base = field == 2 ? base / 2 + 1 : base;
            mcd212_video_begin_frame((uint16_t)field_base, 6, interlaced);
            lle_mcd212_video_begin_frame((uint16_t)field_base, 6, interlaced);
            for (line_id = field & 1; line_id < 7; line_id += interlaced ? 2 : 1) {
                for (unsigned i = 0; i < 2048; i++) {
                    g_ram0[0x1000 + i] = (uint8_t)random_word();
                    g_ram1[0x2000 + i] = (uint8_t)random_word();
                }
                compare_line(0x1000, CDI_RAM1_BASE + 0x2000,
                             (uint16_t)field_base, (uint16_t)field_base);
            }
            compare_frame();
            if (field == 1) compare_frame(); /* identical field, no draw work */
        }
    }
    CHECK(fused != 0);
    printf("scanout contract: PASS %u jobs, %u direct-source observations\n", jobs, fused);
    return 0;
}
