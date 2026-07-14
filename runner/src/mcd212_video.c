/*
 * mcd212_video.c - clean-room Philips MCD212 video decoding/composition.
 *
 * Behavioural reference: Green Book video semantics as exercised by CeDImu.
 * This is an independent C implementation; no CeDImu renderer code is linked
 * or copied into the runtime.
 */
#include "mcd212_video.h"
#include "cdi_runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t g_ram0[CDI_RAM0_SIZE];
extern uint8_t g_ram1[CDI_RAM1_SIZE];

enum { PLANE_A = 0, PLANE_B = 1 };
enum { ICM_OFF, ICM_RGB555, ICM_DYUV, ICM_CLUT8, ICM_CLUT7, ICM_CLUT77, ICM_CLUT4 };
enum { IMAGE_NORMAL, IMAGE_RUN_LENGTH, IMAGE_MOSAIC };
enum { BPP_NORMAL8, BPP_DOUBLE4, BPP_HIGH8 };

typedef struct {
    uint32_t clut[256];
    uint32_t dyuv_start[2];
    uint32_t transparent_color[2];
    uint32_t mask_color[2];
    uint32_t matte[8];
    uint16_t cursor_pattern[16];
    uint16_t cursor_x, cursor_y;
    uint8_t coding[2], image_type[2], bpp[2], pixel_repeat[2];
    uint8_t transparency[2], icf[2], hold_factor[2];
    uint8_t clut_bank, backdrop, cursor_color;
    uint8_t plane_b_front, mix, clut_select_high, two_mattes;
    uint8_t external_video, hold_enabled[2], cursor_enabled;
    uint8_t cursor_double, cursor_blink_type, cursor_on, cursor_off;
    uint64_t cursor_epoch;
} VideoState;

static VideoState s;
static uint32_t s_plane[2][MCD212_VIDEO_MAX_WIDTH];
static uint8_t s_icf_lut[64][256];
static uint32_t s_frame[2][MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT];
static uint16_t s_width, s_height;
static uint16_t s_front_width, s_front_height;
static int s_draw_buffer;
static _Atomic int s_front_buffer;
static _Atomic uint64_t s_generation;
static _Atomic uint64_t s_publish_seq;

static void video_fault(const char *what, uint32_t value) {
    fprintf(stderr, "[mcd212-video] %s ($%08X)\n", what, value);
    abort();
}

static uint8_t dram8(uint32_t address) {
    if (address < CDI_RAM0_SIZE) return g_ram0[address];
    if (address >= CDI_RAM1_BASE && address - CDI_RAM1_BASE < CDI_RAM1_SIZE)
        return g_ram1[address - CDI_RAM1_BASE];
    video_fault("display fetch outside MCD212 DRAM", address);
    return 0;
}

static uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static uint32_t yrgb(uint8_t code) {
    uint8_t level = (code & 8) ? 255 : 127;
    return rgb((code & 4) ? level : 0, (code & 2) ? level : 0,
               (code & 1) ? level : 0);
}

static uint8_t decode_icm_a(uint8_t v) {
    switch (v) {
    case 1: return ICM_CLUT8;
    case 3: return ICM_CLUT7;
    case 4: return ICM_CLUT77;
    case 5: return ICM_DYUV;
    case 11: return ICM_CLUT4;
    default: return ICM_OFF;
    }
}

static uint8_t decode_icm_b(uint8_t v) {
    switch (v) {
    case 1: return ICM_RGB555;
    case 3: return ICM_CLUT7;
    case 5: return ICM_DYUV;
    case 11: return ICM_CLUT4;
    default: return ICM_OFF;
    }
}

void mcd212_video_reset(void) {
    memset(&s, 0, sizeof s);
    memset(s_plane, 0, sizeof s_plane);
    memset(s_frame, 0, sizeof s_frame);
    for (unsigned factor = 0; factor < 64; factor++)
        for (unsigned component = 0; component < 256; component++)
            s_icf_lut[factor][component] =
                clamp8(((int)factor * ((int)component - 16)) / 63 + 16);
    s.image_type[0] = s.image_type[1] = IMAGE_NORMAL;
    s.bpp[0] = s.bpp[1] = BPP_NORMAL8;
    s.pixel_repeat[0] = s.pixel_repeat[1] = 1;
    s.hold_factor[0] = s.hold_factor[1] = 1;
    s.mix = 1;
    s_draw_buffer = 1;
    atomic_store_explicit(&s_front_buffer, 0, memory_order_relaxed);
    atomic_store_explicit(&s_generation, 0, memory_order_relaxed);
    atomic_store_explicit(&s_publish_seq, 0, memory_order_relaxed);
    s_width = 768;
    s_height = 280;
    s_front_width = s_width;
    s_front_height = s_height;
}

void mcd212_video_control(int plane, uint32_t instruction) {
    uint8_t code = (uint8_t)(instruction >> 24);
    uint32_t data = instruction & 0x00FFFFFFu;
    if (plane != PLANE_A && plane != PLANE_B)
        video_fault("invalid control-program plane", (uint32_t)plane);

    if (code >= 0x80 && code <= 0xBF) {
        uint8_t index = (uint8_t)((s.clut_bank << 6) + code - 0x80);
        if (plane == PLANE_A || s.clut_bank >= 2)
            s.clut[index] = 0xFF000000u | data;
        return;
    }
    if (code >= 0xD0 && code <= 0xD7) {
        s.matte[code - 0xD0] = data;
        return;
    }

    switch (code) {
    case 0x10: case 0x20: case 0x40: case 0x60: case 0x70:
        return;
    case 0x78: {
        uint8_t type = (uint8_t)(instruction & 3);
        s.image_type[plane] = type <= 1 ? IMAGE_NORMAL :
                              type == 2 ? IMAGE_RUN_LENGTH : IMAGE_MOSAIC;
        s.pixel_repeat[plane] = (uint8_t)(1u << (1u + ((instruction >> 2) & 3u)));
        switch ((instruction >> 8) & 3u) {
        case 0: s.bpp[plane] = BPP_NORMAL8; break;
        case 1: s.bpp[plane] = BPP_DOUBLE4; break;
        case 2: s.bpp[plane] = BPP_HIGH8; break;
        default: video_fault("reserved display bits-per-pixel", instruction);
        }
        return;
    }
    case 0xC0:
        if (plane == PLANE_A) {
            s.clut_select_high = (uint8_t)((instruction >> 22) & 1);
            s.two_mattes = (uint8_t)((instruction >> 19) & 1);
            s.external_video = (uint8_t)((instruction >> 18) & 1);
            s.coding[PLANE_A] = decode_icm_a((uint8_t)instruction & 15);
            s.coding[PLANE_B] = decode_icm_b((uint8_t)(instruction >> 8) & 15);
        }
        return;
    case 0xC1:
        if (plane == PLANE_A) {
            s.mix = (uint8_t)!((instruction >> 23) & 1);
            s.transparency[PLANE_A] = (uint8_t)instruction & 15;
            s.transparency[PLANE_B] = (uint8_t)(instruction >> 8) & 15;
        }
        return;
    case 0xC2: if (plane == PLANE_A) s.plane_b_front = (uint8_t)instruction & 1; return;
    case 0xC3: s.clut_bank = (uint8_t)instruction & 3; return;
    case 0xC4: if (plane == PLANE_A) s.transparent_color[0] = data; return;
    case 0xC6: if (plane == PLANE_B) s.transparent_color[1] = data; return;
    case 0xC7: if (plane == PLANE_A) s.mask_color[0] = data; return;
    case 0xC9: if (plane == PLANE_B) s.mask_color[1] = data; return;
    case 0xCA: if (plane == PLANE_A) s.dyuv_start[0] = data; return;
    case 0xCB: if (plane == PLANE_B) s.dyuv_start[1] = data; return;
    case 0xCD:
        if (plane == PLANE_A) {
            s.cursor_x = (uint16_t)(instruction & 0x3FF);
            s.cursor_y = (uint16_t)((instruction >> 12) & 0x3FF);
        }
        return;
    case 0xCE:
        if (plane == PLANE_A) {
            s.cursor_color = (uint8_t)instruction & 15;
            s.cursor_enabled = (uint8_t)((instruction >> 23) & 1);
            s.cursor_blink_type = (uint8_t)((instruction >> 22) & 1);
            s.cursor_on = (uint8_t)((instruction >> 19) & 7);
            s.cursor_off = (uint8_t)((instruction >> 16) & 7);
            s.cursor_double = (uint8_t)((instruction >> 15) & 1);
            if (!s.cursor_on && s.cursor_off)
                video_fault("zero cursor-on period with blinking enabled", instruction);
            s.cursor_epoch = atomic_load_explicit(&s_generation, memory_order_relaxed);
        }
        return;
    case 0xCF:
        if (plane == PLANE_A)
            s.cursor_pattern[(instruction >> 16) & 15] = (uint16_t)instruction;
        return;
    case 0xD8: if (plane == PLANE_A) s.backdrop = (uint8_t)instruction & 15; return;
    case 0xD9:
        if (plane == PLANE_A) {
            s.hold_enabled[0] = (uint8_t)((instruction >> 23) & 1);
            s.hold_factor[0] = (uint8_t)instruction;
        }
        return;
    case 0xDA:
        if (plane == PLANE_B) {
            s.hold_enabled[1] = (uint8_t)((instruction >> 23) & 1);
            s.hold_factor[1] = (uint8_t)instruction;
        }
        return;
    case 0xDB: if (plane == PLANE_A) s.icf[0] = (uint8_t)instruction & 63; return;
    case 0xDC: if (plane == PLANE_B) s.icf[1] = (uint8_t)instruction & 63; return;
    default:
        /* Reserved control opcodes are defined as no-ops by the device. */
        return;
    }
}

void mcd212_video_begin_frame(uint16_t base_width, uint16_t base_height,
                              int interlaced) {
    uint32_t width = (uint32_t)base_width * 2u;
    uint32_t height = interlaced ? (uint32_t)base_height * 2u : base_height;
    if (width > MCD212_VIDEO_MAX_WIDTH || height > MCD212_VIDEO_MAX_HEIGHT)
        video_fault("invalid display geometry", (width << 16) | height);
    s_width = (uint16_t)width;
    s_height = (uint16_t)height;
}

static uint32_t clut_pixel(int plane, unsigned index) {
    unsigned base = plane == PLANE_B ? 128u :
                    (s.coding[PLANE_A] == ICM_CLUT77 && s.clut_select_high ? 128u : 0u);
    return s.clut[(base + index) & 255u];
}

static uint16_t decode_clut_bitmap(int plane, uint32_t address, uint16_t source_width,
                                   int duplicate) {
    uint32_t *dst = s_plane[plane];
    unsigned out = 0;
    if (s.coding[plane] == ICM_CLUT4) {
        unsigned bytes = (source_width + 1u) / 2u;
        for (unsigned i = 0; i < bytes; i++) {
            uint8_t b = dram8(address + i);
            dst[out++] = clut_pixel(plane, b >> 4);
            if (out < source_width) dst[out++] = clut_pixel(plane, b & 15);
        }
        return (uint16_t)bytes;
    }
    unsigned mask = s.coding[plane] == ICM_CLUT8 ? 255u : 127u;
    for (unsigned i = 0; i < source_width; i++) {
        uint32_t p = clut_pixel(plane, dram8(address + i) & mask);
        dst[out++] = p;
        if (duplicate) dst[out++] = p;
    }
    return source_width;
}

static uint16_t decode_rgb555(uint32_t address_a, uint32_t address_b,
                              uint16_t source_width) {
    uint32_t *dst = s_plane[PLANE_B];
    unsigned out = 0;
    for (unsigned i = 0; i < source_width; i++) {
        uint16_t v = (uint16_t)((uint16_t)dram8(address_a + i) << 8) |
                     dram8(address_b + i);
        uint32_t p = ((v & 0x8000) ? 0x80000000u : 0) |
                     ((uint32_t)((v >> 7) & 0xF8) << 16) |
                     ((uint32_t)((v >> 2) & 0xF8) << 8) |
                     ((uint32_t)(v << 3) & 0xF8);
        dst[out++] = p;
        dst[out++] = p;
    }
    return source_width;
}

static uint16_t decode_dyuv(int plane, uint32_t address, uint16_t source_width,
                            int duplicate) {
    static const uint8_t delta[16] = {0,1,4,9,16,27,44,79,128,177,212,229,240,247,252,255};
    uint32_t *dst = s_plane[plane];
    uint32_t init = s.dyuv_start[plane];
    uint8_t y = (uint8_t)(init >> 16), u = (uint8_t)(init >> 8), v = (uint8_t)init;
    unsigned out = 0;
    for (unsigned i = 0; i < source_width; i += 2) {
        uint8_t a = dram8(address + i), b = dram8(address + i + 1);
        uint8_t new_u = (uint8_t)(u + delta[a >> 4]);
        uint8_t new_v = (uint8_t)(v + delta[b >> 4]);
        uint8_t u1 = (uint8_t)(((unsigned)u + new_u) >> 1);
        uint8_t v1 = (uint8_t)(((unsigned)v + new_v) >> 1);
        uint8_t y1 = (uint8_t)(y + delta[a & 15]);
        uint8_t y2 = (uint8_t)(y1 + delta[b & 15]);
        uint32_t p1 = rgb(clamp8((int)y1 + 351 * ((int)v1 - 128) / 256),
                          clamp8((int)y1 - 86 * ((int)u1 - 128) / 256 - 179 * ((int)v1 - 128) / 256),
                          clamp8((int)y1 + 444 * ((int)u1 - 128) / 256));
        uint32_t p2 = rgb(clamp8((int)y2 + 351 * ((int)new_v - 128) / 256),
                          clamp8((int)y2 - 86 * ((int)new_u - 128) / 256 - 179 * ((int)new_v - 128) / 256),
                          clamp8((int)y2 + 444 * ((int)new_u - 128) / 256));
        dst[out++] = p1; if (duplicate) dst[out++] = p1;
        if (i + 1 < source_width) {
            dst[out++] = p2; if (duplicate) dst[out++] = p2;
        }
        y = y2; u = new_u; v = new_v;
    }
    return (uint16_t)((source_width + 1u) & ~1u);
}

static uint16_t decode_run_length(int plane, uint32_t address, uint16_t source_width) {
    uint32_t *dst = s_plane[plane];
    unsigned consumed = 0, x = 0;
    int rl3 = s.bpp[plane] == BPP_DOUBLE4;
    int duplicate = !rl3 && source_width <= 384;
    while (x < source_width) {
        uint8_t format = dram8(address + consumed++);
        if (rl3) {
            unsigned pairs = 1;
            if (format & 0x80) {
                pairs = dram8(address + consumed++);
                if (!pairs) pairs = (source_width - x) / 2u;
            }
            uint32_t p0 = clut_pixel(plane, (format >> 4) & 7);
            uint32_t p1 = clut_pixel(plane, format & 7);
            while (pairs-- && x + 1 < source_width) {
                dst[x++] = p0; dst[x++] = p1;
            }
        } else {
            unsigned count = 1;
            if (format & 0x80) {
                count = dram8(address + consumed++);
                if (!count) count = source_width - x;
            }
            uint32_t p = clut_pixel(plane, format & 0x7F);
            while (count-- && x < source_width) {
                if (duplicate) { dst[x * 2] = p; dst[x * 2 + 1] = p; }
                else dst[x] = p;
                x++;
            }
        }
    }
    return (uint16_t)consumed;
}

static uint16_t decode_plane(int plane, uint32_t address, uint32_t address_a,
                             uint16_t source_width) {
    memset(s_plane[plane], 0, sizeof s_plane[plane]);
    if (s.coding[plane] == ICM_OFF) return 0;
    /* CLUT4 and RL3 always carry a double-resolution pixel stream. RL7 high
     * does likewise; normal RL7 remains base-width and is doubled on output. */
    if (((s.image_type[plane] == IMAGE_NORMAL || s.image_type[plane] == IMAGE_MOSAIC) &&
         s.coding[plane] == ICM_CLUT4) ||
        (s.image_type[plane] == IMAGE_RUN_LENGTH &&
         (s.bpp[plane] == BPP_DOUBLE4 || s.bpp[plane] == BPP_HIGH8)))
        source_width = s_width;
    if (s.image_type[plane] == IMAGE_RUN_LENGTH)
        return decode_run_length(plane, address, source_width);
    unsigned mosaic = s.image_type[plane] == IMAGE_MOSAIC ? s.pixel_repeat[plane] : 1u;
    unsigned full_source_width = source_width;
    int duplicate = s.coding[plane] != ICM_CLUT4 && full_source_width <= 384u;
    if (mosaic > 1u)
        source_width = (uint16_t)((full_source_width + mosaic - 1u) / mosaic);
    uint16_t bytes;
    switch (s.coding[plane]) {
    case ICM_RGB555: bytes = decode_rgb555(address_a, address, source_width); break;
    case ICM_DYUV: bytes = decode_dyuv(plane, address, source_width, duplicate); break;
    case ICM_CLUT8: case ICM_CLUT7: case ICM_CLUT77: case ICM_CLUT4:
        bytes = decode_clut_bitmap(plane, address, source_width, duplicate); break;
    default: return 0;
    }
    if (mosaic > 1u) {
        uint32_t *dst = s_plane[plane];
        for (unsigned x = s_width; x-- > 0; ) dst[x] = dst[x / mosaic];
    }
    return bytes;
}

static void apply_pixel_hold(int plane) {
    if (!s.hold_enabled[plane]) return;
    unsigned factor = s.hold_factor[plane];
    if (!factor) video_fault("reserved zero pixel-hold factor", (uint32_t)plane);
    uint32_t *pixels = s_plane[plane];
    for (unsigned x = 0; x < s_width; x += factor) {
        uint32_t held = pixels[x];
        unsigned end = x + factor < s_width ? x + factor : s_width;
        for (unsigned i = x + 1; i < end; i++) pixels[i] = held;
    }
}

static int color_key(int plane, uint32_t p) {
    const uint32_t mask = s.mask_color[plane];
    /* Mask bits force the corresponding comparison bits equal; they do not
     * select bits with AND.  Component bits 0..1 are not compared by the
     * MCD212 color key (Green Book V.5.7.2.2). */
    return (((p | mask) & 0x00FCFCFCu) ==
            ((s.transparent_color[plane] | mask) & 0x00FCFCFCu));
}

static uint8_t icf_component(uint8_t c, uint8_t factor) {
    return s_icf_lut[factor & 63u][c];
}

static uint32_t apply_icf(uint32_t p, uint8_t factor) {
    return (p & 0xFF000000u) |
           ((uint32_t)icf_component((uint8_t)(p >> 16), factor) << 16) |
           ((uint32_t)icf_component((uint8_t)(p >> 8), factor) << 8) |
           icf_component((uint8_t)p, factor);
}

static int is_transparent(int plane, uint32_t p, const uint8_t matte_flags[2]) {
    uint8_t control = s.transparency[plane];
    int condition;
    switch (control & 7) {
    /* Mode 0 is the constant (Always/Never) predicate.  The predicate itself
     * is true; bit 3 selects its inverse.  Thus TC=0 is always transparent
     * and TC=8 is never transparent (Green Book V.5.8.1). */
    case 0: condition = 1; break;
    case 1: condition = color_key(plane, p); break;
    case 2: condition = (p >> 31) & 1; break;
    case 3: condition = matte_flags[0]; break;
    case 4: condition = matte_flags[1]; break;
    case 5: condition = matte_flags[0] || color_key(plane, p); break;
    case 6: condition = matte_flags[1] || color_key(plane, p); break;
    default: condition = 0; break;
    }
    return condition == !(control & 8);
}

static uint32_t mix_pixels(uint32_t a, uint32_t b) {
    return rgb(clamp8((int)((a >> 16) & 255) + (int)((b >> 16) & 255) - 16),
               clamp8((int)((a >> 8) & 255) + (int)((b >> 8) & 255) - 16),
               clamp8((int)(a & 255) + (int)(b & 255) - 16));
}

static void composite_line(uint16_t line) {
    if (line >= s_height) return;
    uint32_t *dst = &s_frame[s_draw_buffer][(uint32_t)line * s_width];
    uint32_t backdrop = yrgb(s.backdrop);
    uint8_t flags[2] = {0, 0};
    uint8_t factors[2] = {s.icf[0], s.icf[1]};
    unsigned matte_pos[2] = {0, s.two_mattes ? 4u : 8u};
    unsigned matte_limit[2] = {s.two_mattes ? 4u : 8u, 8u};
    if (!s.two_mattes) {
        /* With one matte, control 0 selects which matte flag it drives. */
        unsigned selected = (s.matte[0] >> 16) & 1u;
        matte_pos[selected] = 0;
        matte_limit[selected] = 8;
        matte_pos[!selected] = matte_limit[!selected] = 8;
    }
    for (unsigned x = 0; x < s_width; x++) {
        /* Matte commands are ordered by x and their state persists. Walk each
         * list once per line instead of rescanning up to eight commands for
         * every output pixel. */
        for (unsigned m = 0; m < 2; m++) {
            while (matte_pos[m] < matte_limit[m]) {
                uint32_t cmd = s.matte[matte_pos[m]];
                if ((cmd & 0x3FFu) > x) break;
                unsigned op = (cmd >> 20) & 15u;
                uint8_t v = (uint8_t)((cmd >> 10) & 63u);
                if (op == 0) { matte_pos[m] = matte_limit[m]; break; }
                if (op == 4 || op == 12 || op == 13) factors[0] = v;
                if (op == 6 || op == 14 || op == 15) factors[1] = v;
                if (op == 8 || op == 12 || op == 14) flags[m] = 0;
                if (op == 9 || op == 13 || op == 15) flags[m] = 1;
                matte_pos[m]++;
            }
        }
        uint32_t a = apply_icf(s_plane[0][x], factors[0]);
        uint32_t b = apply_icf(s_plane[1][x], factors[1]);
        int ta = is_transparent(0, s_plane[0][x], flags);
        int tb = is_transparent(1, s_plane[1][x], flags);
        uint32_t front = s.plane_b_front ? b : a;
        uint32_t back  = s.plane_b_front ? a : b;
        int tf = s.plane_b_front ? tb : ta;
        int tback = s.plane_b_front ? ta : tb;
        if (s.mix && !ta && !tb) dst[x] = mix_pixels(a, b);
        else if (!tf) dst[x] = front | 0xFF000000u;
        else if (!tback) dst[x] = back | 0xFF000000u;
        else dst[x] = backdrop;
    }

    if (s.cursor_enabled && line >= s.cursor_y && line < s.cursor_y + 16u) {
        uint64_t field = atomic_load_explicit(&s_generation, memory_order_relaxed);
        int cursor_visible = 1, cursor_complement = 0;
        if (s.cursor_off) {
            uint64_t on_fields = 12u * s.cursor_on;
            uint64_t off_fields = 12u * s.cursor_off;
            uint64_t phase = (field - s.cursor_epoch) % (on_fields + off_fields);
            if (phase >= on_fields) {
                cursor_visible = s.cursor_blink_type;
                cursor_complement = s.cursor_blink_type;
            }
        }
        if (!cursor_visible) return;
        unsigned cy = line - s.cursor_y;
        uint16_t pattern = s.cursor_pattern[cy];
        uint32_t color = yrgb(s.cursor_color);
        if (cursor_complement) color ^= 0x00FFFFFFu;
        unsigned scale = s.cursor_double ? 1u : 2u;
        for (unsigned cx = 0; cx < 16; cx++) {
            if (!(pattern & (0x8000u >> cx))) continue;
            unsigned x = s.cursor_x + cx * scale;
            for (unsigned i = 0; i < scale && x + i < s_width; i++) dst[x + i] = color;
        }
    }
}

void mcd212_video_draw_line(uint32_t address_a, uint32_t address_b,
                            uint16_t line, uint16_t source_width_a,
                            uint16_t source_width_b,
                            uint16_t *bytes_a, uint16_t *bytes_b) {
    uint16_t a = decode_plane(PLANE_A, address_a, address_a, source_width_a);
    uint16_t b = decode_plane(PLANE_B, address_b, address_a, source_width_b);
    if (s.coding[PLANE_B] == ICM_RGB555) a = b;
    apply_pixel_hold(PLANE_A);
    apply_pixel_hold(PLANE_B);
    composite_line(line);
    if (bytes_a) *bytes_a = a;
    if (bytes_b) *bytes_b = b;
}

void mcd212_video_end_frame(void) {
    const int published = s_draw_buffer;
    atomic_fetch_add_explicit(&s_publish_seq, 1, memory_order_acq_rel);
    atomic_store_explicit(&s_front_buffer, published, memory_order_release);
    s_front_width = s_width;
    s_front_height = s_height;
    atomic_fetch_add_explicit(&s_generation, 1, memory_order_release);
    atomic_fetch_add_explicit(&s_publish_seq, 1, memory_order_release);
    s_draw_buffer ^= 1;

    /* The MCD212/CeDImu screen is persistent: if DE is clear for a line (or
     * changes partway through a field), that line keeps the pixel data from
     * the immediately preceding completed field.  A host-facing double buffer
     * must seed its new back buffer from the frame it just published; merely
     * flipping buffers resurrects two-fields-old pixels on every undrawn line.
     * Copy the full fixed-size plane so a later geometry expansion has the
     * same retained backing-store contents as the device's persistent Plane. */
    memcpy(s_frame[s_draw_buffer], s_frame[published], sizeof s_frame[published]);
}

uint32_t mcd212_video_copy_frame(uint32_t *dst, uint32_t capacity,
                                 uint16_t *width, uint16_t *height,
                                 uint64_t *generation) {
    uint64_t before, after, gen = 0;
    uint16_t w = 0, h = 0;
    uint32_t count = 0;
    do {
        before = atomic_load_explicit(&s_publish_seq, memory_order_acquire);
        if (before & 1u) {
            after = before + 1u;
            continue;
        }
        w = s_front_width;
        h = s_front_height;
        count = (uint32_t)w * h;
        if (dst) {
            if (capacity < count) video_fault("framebuffer destination too small", capacity);
            int front = atomic_load_explicit(&s_front_buffer, memory_order_acquire);
            memcpy(dst, s_frame[front], (size_t)count * sizeof(uint32_t));
        }
        gen = atomic_load_explicit(&s_generation, memory_order_acquire);
        after = atomic_load_explicit(&s_publish_seq, memory_order_acquire);
    } while (before != after || (after & 1u));
    if (width) *width = w;
    if (height) *height = h;
    if (generation) *generation = gen;
    return count;
}

uint64_t mcd212_video_frame_hash(uint16_t *width, uint16_t *height,
                                 uint64_t *generation) {
    const uint64_t basis = 0x14650FB0739D0383ULL;
    const uint64_t prime = 0x00000100000001B3ULL;
    uint64_t before, after, hash, gen = 0;
    uint16_t w = 0, h = 0;
    do {
        before = atomic_load_explicit(&s_publish_seq, memory_order_acquire);
        if (before & 1u) {
            after = before + 1u;
            continue;
        }
        int front = atomic_load_explicit(&s_front_buffer, memory_order_acquire);
        const uint8_t *bytes = (const uint8_t *)s_frame[front];
        w = s_front_width;
        h = s_front_height;
        size_t count = (size_t)w * h * sizeof(uint32_t);
        hash = basis;
        for (size_t i = 0; i < count; i++) {
            hash ^= bytes[i];
            hash *= prime;
        }
        gen = atomic_load_explicit(&s_generation, memory_order_acquire);
        after = atomic_load_explicit(&s_publish_seq, memory_order_acquire);
    } while (before != after || (after & 1u));
    if (width) *width = w;
    if (height) *height = h;
    if (generation) *generation = gen;
    return hash;
}

void mcd212_video_debug_cursor(Mcd212VideoCursorState *out) {
    if (!out) return;
    out->x = s.cursor_x;
    out->y = s.cursor_y;
    memcpy(out->pattern, s.cursor_pattern, sizeof out->pattern);
    out->enabled = s.cursor_enabled;
    out->color = s.cursor_color;
    out->double_resolution = s.cursor_double;
    out->blink_type = s.cursor_blink_type;
    out->blink_on = s.cursor_on;
    out->blink_off = s.cursor_off;
}

void mcd212_video_debug_state(Mcd212VideoDebugState *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    memcpy(out->coding, s.coding, sizeof out->coding);
    memcpy(out->image_type, s.image_type, sizeof out->image_type);
    memcpy(out->bpp, s.bpp, sizeof out->bpp);
    memcpy(out->pixel_repeat, s.pixel_repeat, sizeof out->pixel_repeat);
    memcpy(out->transparency, s.transparency, sizeof out->transparency);
    memcpy(out->icf, s.icf, sizeof out->icf);
    memcpy(out->hold_enabled, s.hold_enabled, sizeof out->hold_enabled);
    memcpy(out->hold_factor, s.hold_factor, sizeof out->hold_factor);
    memcpy(out->dyuv_start, s.dyuv_start, sizeof out->dyuv_start);
    memcpy(out->transparent_color, s.transparent_color, sizeof out->transparent_color);
    memcpy(out->mask_color, s.mask_color, sizeof out->mask_color);
    out->clut_bank = s.clut_bank;
    out->backdrop = s.backdrop;
    out->plane_b_front = s.plane_b_front;
    out->mix = s.mix;
    out->clut_select_high = s.clut_select_high;
    out->two_mattes = s.two_mattes;
    out->external_video = s.external_video;
    const uint8_t *bytes = (const uint8_t *)s.clut;
    out->clut_hash = 0x14650FB0739D0383ULL;
    for (size_t i = 0; i < sizeof s.clut; i++) {
        out->clut_hash ^= bytes[i];
        out->clut_hash *= 0x00000100000001B3ULL;
    }
}
