#include "mcd212_video.h"
#include "cdi_runtime.h"

#include <stdio.h>
#include <string.h>

uint8_t g_ram0[CDI_RAM0_SIZE];
uint8_t g_ram1[CDI_RAM1_SIZE];

static uint32_t frame[MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT];
static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } \
} while (0)
#define CHECK_PIXEL(index, expected) do { \
    uint32_t actual_ = frame[(index)], expected_ = (expected); \
    if (actual_ != expected_) { \
        fprintf(stderr, "FAIL %s:%d: frame[%u]=$%08X expected $%08X\n", \
                __FILE__, __LINE__, (unsigned)(index), actual_, expected_); \
        failures++; \
    } \
} while (0)

static void begin(void) {
    memset(g_ram0, 0, sizeof g_ram0);
    memset(g_ram1, 0, sizeof g_ram1);
    memset(frame, 0, sizeof frame);
    mcd212_video_reset();
    mcd212_video_begin_frame(360, 1, 0);
    mcd212_video_control(0, 0xDB00003Fu); /* plane A contribution = unity */
    mcd212_video_control(0, 0xC1000808u); /* both planes never transparent */
}

static void finish(uint16_t *a, uint16_t *b) {
    uint16_t w = 0, h = 0;
    uint64_t generation = 0;
    mcd212_video_draw_line(0, CDI_RAM1_BASE, 0, 360, 360, a, b);
    mcd212_video_end_frame();
    CHECK(mcd212_video_copy_frame(frame, sizeof frame / sizeof frame[0],
                                  &w, &h, &generation) == 720);
    CHECK(w == 720 && h == 1 && generation == 1);
}

static void test_clut8(void) {
    uint16_t a, b;
    begin();
    mcd212_video_control(0, 0x81112233u);
    mcd212_video_control(0, 0xC0000001u);
    memset(g_ram0, 1, 360);
    finish(&a, &b);
    CHECK(a == 360 && b == 0);
    CHECK_PIXEL(0, 0xFF112233u); CHECK_PIXEL(1, 0xFF112233u);
    CHECK_PIXEL(719, 0xFF112233u);
}

static void test_clut4(void) {
    uint16_t a, b;
    begin();
    mcd212_video_control(0, 0x81AA0000u);
    mcd212_video_control(0, 0x8200BB00u);
    mcd212_video_control(0, 0xC000000Bu);
    memset(g_ram0, 0x12, 360);
    finish(&a, &b);
    CHECK(a == 360 && b == 0);
    CHECK_PIXEL(0, 0xFFAA0000u); CHECK_PIXEL(1, 0xFF00BB00u);
    CHECK_PIXEL(718, 0xFFAA0000u); CHECK_PIXEL(719, 0xFF00BB00u);
}

static void test_rl7(void) {
    uint16_t a, b;
    begin();
    mcd212_video_control(0, 0x81224466u);
    mcd212_video_control(0, 0xC0000003u);
    mcd212_video_control(0, 0x78000002u);
    g_ram0[0] = 0x81; g_ram0[1] = 0;
    finish(&a, &b);
    CHECK(a == 2 && b == 0);
    CHECK_PIXEL(0, 0xFF224466u); CHECK_PIXEL(719, 0xFF224466u);

}

static void test_rl3(void) {
    uint16_t a, b;
    begin();
    mcd212_video_control(0, 0x81CC0000u);
    mcd212_video_control(0, 0x820000DDu);
    mcd212_video_control(0, 0xC000000Bu);
    mcd212_video_control(0, 0x78000102u);
    g_ram0[0] = 0x92; g_ram0[1] = 0;
    finish(&a, &b);
    CHECK(a == 2 && b == 0);
    CHECK_PIXEL(0, 0xFFCC0000u); CHECK_PIXEL(1, 0xFF0000DDu);
    CHECK_PIXEL(718, 0xFFCC0000u); CHECK_PIXEL(719, 0xFF0000DDu);

}

static void test_rgb555(void) {
    uint16_t a, b;
    begin();
    mcd212_video_control(0, 0xDB000000u); /* off plane A contributes black level */
    mcd212_video_control(1, 0xDC00003Fu); /* RGB555 plane B contribution = unity */
    mcd212_video_control(0, 0xC0000100u);
    memset(g_ram0, 0xFC, 360);
    memset(g_ram1, 0x00, 360);
    finish(&a, &b);
    CHECK(a == 360 && b == 360);
    CHECK_PIXEL(0, 0xFFF80000u); CHECK_PIXEL(719, 0xFFF80000u);
}

static void test_dyuv(void) {
    uint16_t a, b;
    begin();
    mcd212_video_control(0, 0xCA808080u);
    mcd212_video_control(0, 0xC0000005u);
    finish(&a, &b);
    CHECK(a == 360 && b == 0);
    CHECK_PIXEL(0, 0xFF808080u); CHECK_PIXEL(719, 0xFF808080u);

    begin();
    mcd212_video_control(0, 0xCA80FA80u);
    mcd212_video_control(0, 0xC0000005u);
    g_ram0[0] = 0x40; /* U: 250 + 16 -> 10; midpoint wraps to 2. */
    g_ram0[1] = 0x00;
    finish(&a, &b);
    CHECK_PIXEL(0, 0xFF80AA00u);
}

static void setup_two_planes(void) {
    mcd212_video_control(0, 0x81CC0000u); /* A CLUT[1] red */
    mcd212_video_control(1, 0xC3000002u); /* B owns CLUT banks 2/3 */
    mcd212_video_control(1, 0x8100CC00u); /* B CLUT[129] green */
    mcd212_video_control(0, 0xC0000301u); /* A CLUT8, B CLUT7 */
    mcd212_video_control(1, 0xDC00003Fu);
    memset(g_ram0, 1, 360);
    memset(g_ram1, 1, 360);
}

static void test_overlay_transparency_and_order(void) {
    uint16_t a, b;
    begin(); setup_two_planes();
    mcd212_video_control(0, 0xC1800808u); /* overlay, both visible, A in front */
    finish(&a, &b);
    CHECK_PIXEL(0, 0xFFCC0000u);

    begin(); setup_two_planes();
    mcd212_video_control(0, 0xC1800808u);
    mcd212_video_control(0, 0xC2000001u); /* B in front */
    finish(&a, &b);
    CHECK_PIXEL(0, 0xFF00CC00u);

    begin(); setup_two_planes();
    mcd212_video_control(0, 0xC4CC0000u);
    mcd212_video_control(0, 0xC7FFFFFFu);
    mcd212_video_control(0, 0xC1800801u); /* A color-key, B never transparent */
    finish(&a, &b);
    CHECK_PIXEL(0, 0xFF00CC00u);

    begin(); setup_two_planes();
    mcd212_video_control(0, 0xC4070707u);
    mcd212_video_control(0, 0xC7000000u); /* zero mask: compare the actual colors */
    mcd212_video_control(0, 0xC1800801u);
    finish(&a, &b);
    CHECK_PIXEL(0, 0xFFCC0000u); /* red does not match $070707 */

    begin(); setup_two_planes();
    mcd212_video_control(0, 0xC4CC0000u);
    mcd212_video_control(0, 0xC7000000u);
    mcd212_video_control(0, 0xC1800801u);
    finish(&a, &b);
    CHECK_PIXEL(0, 0xFF00CC00u); /* exact red key exposes plane B */
}

static void test_matte_region(void) {
    uint16_t a, b;
    begin(); setup_two_planes();
    mcd212_video_control(0, 0xC1800803u); /* A matte flag 0, B never transparent */
    mcd212_video_control(0, 0xD0900064u); /* set flag 0 at x=100 */
    finish(&a, &b);
    CHECK_PIXEL(99, 0xFFCC0000u);
    CHECK_PIXEL(100, 0xFF00CC00u);
    CHECK_PIXEL(719, 0xFF00CC00u);
}

static void test_always_never_transparency(void) {
    uint16_t a, b;
    begin();
    mcd212_video_control(0, 0x81112233u);
    mcd212_video_control(0, 0xC0000001u);
    mcd212_video_control(0, 0xD800000Fu); /* white backdrop */
    mcd212_video_control(0, 0xC1000000u); /* both planes always transparent */
    memset(g_ram0, 1, 360);
    finish(&a, &b);
    CHECK_PIXEL(0, 0xFFFFFFFFu);

    begin();
    mcd212_video_control(0, 0x81112233u);
    mcd212_video_control(0, 0xC0000001u);
    memset(g_ram0, 1, 360);
    finish(&a, &b);
    CHECK_PIXEL(0, 0xFF112233u); /* begin() programs TC=8: never transparent */
}

static void test_cursor(void) {
    uint16_t a, b;
    Mcd212VideoCursorState cursor;
    begin();
    mcd212_video_control(0, 0xCD02A155u); /* cursor x=$155, y=$2A */
    mcd212_video_control(0, 0xCE80800Fu); /* enable, double-res, white YRGB */
    mcd212_video_control(0, 0xCF008001u); /* line 0 pattern $8001 */
    mcd212_video_debug_cursor(&cursor);
    CHECK(cursor.x == 0x155 && cursor.y == 0x2A && cursor.enabled == 1);
    CHECK(cursor.color == 15 && cursor.double_resolution == 1);
    CHECK(cursor.pattern[0] == 0x8001);
    mcd212_video_control(0, 0xCD000000u);
    finish(&a, &b);
    CHECK_PIXEL(0, 0xFFFFFFFFu);
    CHECK_PIXEL(1, 0xFF000000u);
    CHECK_PIXEL(15, 0xFFFFFFFFu);
}

static void test_cursor_normal_and_blink(void) {
    uint16_t a = 0, b = 0, w, h;
    uint64_t generation;
    begin();
    mcd212_video_control(0, 0xCE80000Fu); /* normal-res: each pattern bit is 2 pixels */
    mcd212_video_control(0, 0xCF008000u);
    finish(&a, &b);
    CHECK_PIXEL(0, 0xFFFFFFFFu); CHECK_PIXEL(1, 0xFFFFFFFFu);
    CHECK_PIXEL(2, 0xFF000000u);

    begin();
    mcd212_video_control(0, 0xCE89800Fu); /* double-res, on 12 fields/off 12 fields */
    mcd212_video_control(0, 0xCF008000u);
    for (unsigned field = 0; field < 13; field++) {
        mcd212_video_draw_line(0, CDI_RAM1_BASE, 0, 360, 360, &a, &b);
        mcd212_video_end_frame();
        mcd212_video_copy_frame(frame, sizeof frame / sizeof frame[0],
                                &w, &h, &generation);
        if (field == 11) CHECK_PIXEL(0, 0xFFFFFFFFu);
        if (field == 12) CHECK_PIXEL(0, 0xFF000000u);
    }

    begin();
    mcd212_video_control(0, 0xCEC9800Cu); /* complementary blink, bright-red cursor */
    mcd212_video_control(0, 0xCF008000u);
    for (unsigned field = 0; field < 13; field++) {
        mcd212_video_draw_line(0, CDI_RAM1_BASE, 0, 360, 360, &a, &b);
        mcd212_video_end_frame();
    }
    mcd212_video_copy_frame(frame, sizeof frame / sizeof frame[0],
                            &w, &h, &generation);
    CHECK_PIXEL(0, 0xFF00FFFFu); /* complement of full-brightness red */
}

static void test_mosaic_file(void) {
    uint16_t a, b;
    begin();
    mcd212_video_control(0, 0x81123456u);
    mcd212_video_control(0, 0x82765432u);
    mcd212_video_control(0, 0xC0000001u);
    mcd212_video_control(0, 0x78000007u); /* mosaic, MF=4 */
    for (unsigned i = 0; i < 90; i++) g_ram0[i] = (uint8_t)(1 + (i & 1));
    finish(&a, &b);
    CHECK(a == 90 && b == 0);
    for (unsigned i = 0; i < 8; i++) CHECK_PIXEL(i, 0xFF123456u);
    for (unsigned i = 8; i < 16; i++) CHECK_PIXEL(i, 0xFF765432u);
}

static void test_pixel_hold(void) {
    uint16_t a, b;
    begin();
    mcd212_video_control(0, 0x81123456u);
    mcd212_video_control(0, 0x82765432u);
    mcd212_video_control(0, 0xC0000001u);
    mcd212_video_control(0, 0xD9800003u); /* enable post-decode hold N=3 */
    for (unsigned i = 0; i < 360; i++) g_ram0[i] = (uint8_t)(1 + (i & 1));
    finish(&a, &b);
    CHECK(a == 360 && b == 0);
    CHECK_PIXEL(0, 0xFF123456u); CHECK_PIXEL(1, 0xFF123456u); CHECK_PIXEL(2, 0xFF123456u);
    CHECK_PIXEL(3, 0xFF765432u); CHECK_PIXEL(4, 0xFF765432u); CHECK_PIXEL(5, 0xFF765432u);
}

int main(void) {
    test_clut8();
    test_clut4();
    test_rl7();
    test_rl3();
    test_rgb555();
    test_dyuv();
    test_overlay_transparency_and_order();
    test_matte_region();
    test_always_never_transparency();
    test_cursor();
    test_cursor_normal_and_blink();
    test_mosaic_file();
    test_pixel_hold();
    if (failures) {
        fprintf(stderr, "mcd212 video tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("mcd212 video tests: PASS");
    return 0;
}
