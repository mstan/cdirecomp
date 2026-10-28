/*
 * MCD212 scanline decoder and compositor.
 *
 * Implemented from the CD-i Full Functional Specification video rules:
 * CLUT/RGB555/DYUV/run-length image coding, mosaic and pixel hold, two-plane
 * transparency/matte/ICF composition, and the hardware cursor.  The module is
 * deterministic and communicates with the host only through published ARGB
 * frames; all source bytes come from emulated MCD212 DRAM.
 */
#include "mcd212_video.h"
#include "cdi_runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CDI_VIDEO_HLE
#define CDI_VIDEO_HLE 0
#endif

enum { PLANE_A, PLANE_B, PLANE_COUNT };
enum {
    CODING_OFF,
    CODING_RGB555,
    CODING_DYUV,
    CODING_CLUT8,
    CODING_CLUT7,
    CODING_CLUT77,
    CODING_CLUT4
};
enum { FILE_BITMAP, FILE_RUN_LENGTH, FILE_MOSAIC };
enum { BITS_NORMAL_8, BITS_DOUBLE_4, BITS_HIGH_8 };

typedef struct {
    uint8_t coding;
    uint8_t file_type;
    uint8_t bits_per_pixel;
    uint8_t mosaic_repeat;
    uint8_t transparency;
    uint8_t contribution;
    uint8_t hold_enabled;
    uint8_t hold_count;
    uint32_t dyuv_initial;
    uint32_t transparent_color;
    uint32_t color_mask;
} PlaneSetup;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t pattern[16];
    uint8_t enabled;
    uint8_t color;
    uint8_t high_resolution;
    uint8_t complementary_blink;
    uint8_t on_units;
    uint8_t off_units;
    uint64_t start_field;
} CursorSetup;

typedef struct {
    PlaneSetup plane[PLANE_COUNT];
    CursorSetup cursor;
    uint32_t clut[256];
    uint32_t matte_command[8];
    uint8_t clut_bank;
    uint8_t backdrop;
    uint8_t plane_b_front;
    uint8_t mixing;
    uint8_t plane_a_high_clut;
    uint8_t two_mattes;
    uint8_t external_video;
} VideoRegisters;

#if CDI_VIDEO_HLE
/* A completed line is reusable only when its entire input contract is equal.
 * Compare authoritative source bytes, rather than maintaining write-side tags.
 * Output/plane/verdict storage already retains the last result for each row. */
enum { LINE_SOURCE_CAPACITY = MCD212_VIDEO_MAX_WIDTH * 2u };
typedef struct {
    VideoRegisters registers;
    uint32_t address[PLANE_COUNT];
    uint16_t width[PLANE_COUNT], consumed[PLANE_COUNT];
    uint8_t source[PLANE_COUNT][LINE_SOURCE_CAPACITY];
    uint32_t decoded[PLANE_COUNT][MCD212_VIDEO_MAX_WIDTH];
    uint8_t valid;
} CompletedLine;
static CompletedLine completed_lines[MCD212_VIDEO_MAX_HEIGHT];
#endif

typedef struct {
    VideoRegisters registers;
    uint32_t decoded[PLANE_COUNT][MCD212_VIDEO_MAX_WIDTH];
    uint8_t contribution_lut[64][256];
    uint32_t frame[2][MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT];
    /* Always-on per-plane debug snapshots (ring-buffer doctrine: the probe
     * queries state that is captured continuously, never armed).  Raw
     * decoded plane pixels before composition, plus a per-pixel verdict
     * mask: bit0 transparent_a, bit1 transparent_b, bit2 output-was-front,
     * bit3 output-was-back, bit4 output-was-backdrop, bit5 mixed. */
    uint32_t plane_frame[PLANE_COUNT][MCD212_VIDEO_MAX_WIDTH *
                                      MCD212_VIDEO_MAX_HEIGHT];
    uint8_t verdict_frame[MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT];
    uint16_t width;
    uint16_t height;
    uint16_t published_width;
    uint16_t published_height;
    int drawing_buffer;
    _Atomic int published_buffer;
    _Atomic uint64_t generation;
    _Atomic uint64_t publish_sequence;
    _Atomic uint64_t published_hash;
    _Atomic uint64_t content_generation;
    uint64_t scanout_jobs;
    uint64_t fused_jobs;
    uint64_t reused_jobs;
} VideoPipeline;

static VideoPipeline video;

static void video_error(const char *message, uint32_t detail) {
    fprintf(stderr, "[mcd212-video] %s ($%08X)\n", message, detail);
    abort();
}

static uint8_t dram_byte(uint32_t address) {
    extern uint8_t g_ram0[CDI_RAM0_SIZE];
    extern uint8_t g_ram1[CDI_RAM1_SIZE];
    if (address < CDI_RAM0_SIZE) return g_ram0[address];
    if (address >= CDI_RAM1_BASE && address - CDI_RAM1_BASE < CDI_RAM1_SIZE)
        return g_ram1[address - CDI_RAM1_BASE];
    video_error("display fetch outside DRAM", address);
    return 0;
}

static uint8_t saturate(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static uint32_t argb(uint8_t red, uint8_t green, uint8_t blue) {
    return 0xFF000000u | ((uint32_t)red << 16) |
           ((uint32_t)green << 8) | blue;
}

static uint32_t yrgb_color(unsigned code) {
    uint8_t level = (code & 8u) ? 255 : 127;
    return argb((code & 4u) ? level : 0,
                (code & 2u) ? level : 0,
                (code & 1u) ? level : 0);
}

static uint8_t plane_a_coding(unsigned code) {
    switch (code) {
    case 1: return CODING_CLUT8;
    case 3: return CODING_CLUT7;
    case 4: return CODING_CLUT77;
    case 5: return CODING_DYUV;
    case 11: return CODING_CLUT4;
    default: return CODING_OFF;
    }
}

static uint8_t plane_b_coding(unsigned code) {
    switch (code) {
    case 1: return CODING_RGB555;
    case 3: return CODING_CLUT7;
    case 5: return CODING_DYUV;
    case 11: return CODING_CLUT4;
    default: return CODING_OFF;
    }
}

void mcd212_video_reset(void) {
    unsigned factor;
    memset(&video, 0, sizeof video);
#if CDI_VIDEO_HLE
    memset(completed_lines, 0, sizeof completed_lines);
#endif
    for (factor = 0; factor < 64; factor++) {
        unsigned component;
        for (component = 0; component < 256; component++)
            video.contribution_lut[factor][component] =
                saturate(((int)factor * ((int)component - 16)) / 63 + 16);
    }
    for (factor = 0; factor < PLANE_COUNT; factor++) {
        video.registers.plane[factor].file_type = FILE_BITMAP;
        video.registers.plane[factor].bits_per_pixel = BITS_NORMAL_8;
        video.registers.plane[factor].mosaic_repeat = 1;
        video.registers.plane[factor].hold_count = 1;
    }
    video.registers.mixing = 1;
    video.drawing_buffer = 1;
    atomic_store_explicit(&video.published_buffer, 0, memory_order_relaxed);
    atomic_store_explicit(&video.generation, 0, memory_order_relaxed);
    atomic_store_explicit(&video.publish_sequence, 0, memory_order_relaxed);
    atomic_store_explicit(&video.published_hash, 0, memory_order_relaxed);
    atomic_store_explicit(&video.content_generation, 0, memory_order_relaxed);
    video.width = video.published_width = 768;
    video.height = video.published_height = 280;
}

static void configure_file_type(int plane, uint32_t instruction) {
    PlaneSetup *setup = &video.registers.plane[plane];
    unsigned type = instruction & 3u;
    unsigned bits = (instruction >> 8) & 3u;

    setup->file_type = (uint8_t)(type <= 1 ? FILE_BITMAP :
                                 type == 2 ? FILE_RUN_LENGTH : FILE_MOSAIC);
    setup->mosaic_repeat = (uint8_t)(1u << (1u + ((instruction >> 2) & 3u)));
    if (bits == 0) setup->bits_per_pixel = BITS_NORMAL_8;
    else if (bits == 1) setup->bits_per_pixel = BITS_DOUBLE_4;
    else if (bits == 2) setup->bits_per_pixel = BITS_HIGH_8;
    else video_error("reserved display bits-per-pixel", instruction);
}

void mcd212_video_control(int plane, uint32_t instruction) {
    VideoRegisters *r = &video.registers;
    uint8_t opcode = (uint8_t)(instruction >> 24);
    uint32_t operand = instruction & 0x00FFFFFFu;

    if (plane < 0 || plane >= PLANE_COUNT)
        video_error("invalid control-program plane", (uint32_t)plane);

    if (opcode >= 0x80 && opcode <= 0xBF) {
        unsigned index = (unsigned)r->clut_bank * 64u + opcode - 0x80u;
        if (plane == PLANE_A || r->clut_bank >= 2)
            r->clut[index] = 0xFF000000u | operand;
        return;
    }
    if (opcode >= 0xD0 && opcode <= 0xD7) {
        r->matte_command[opcode - 0xD0] = operand;
        return;
    }

    switch (opcode) {
    case 0x10: case 0x20: case 0x40: case 0x60: case 0x70:
        break;
    case 0x78:
        configure_file_type(plane, instruction);
        break;
    case 0xC0:
        if (plane == PLANE_A) {
            r->plane_a_high_clut = (uint8_t)((instruction >> 22) & 1u);
            r->two_mattes = (uint8_t)((instruction >> 19) & 1u);
            r->external_video = (uint8_t)((instruction >> 18) & 1u);
            r->plane[PLANE_A].coding = plane_a_coding(instruction & 15u);
            r->plane[PLANE_B].coding = plane_b_coding((instruction >> 8) & 15u);
        }
        break;
    case 0xC1:
        if (plane == PLANE_A) {
            r->mixing = (uint8_t)!((instruction >> 23) & 1u);
            r->plane[PLANE_A].transparency = (uint8_t)(instruction & 15u);
            r->plane[PLANE_B].transparency = (uint8_t)((instruction >> 8) & 15u);
        }
        break;
    case 0xC2:
        if (plane == PLANE_A) r->plane_b_front = (uint8_t)(instruction & 1u);
        break;
    case 0xC3:
        r->clut_bank = (uint8_t)(instruction & 3u);
        break;
    case 0xC4:
        if (plane == PLANE_A) r->plane[PLANE_A].transparent_color = operand;
        break;
    case 0xC6:
        if (plane == PLANE_B) r->plane[PLANE_B].transparent_color = operand;
        break;
    case 0xC7:
        if (plane == PLANE_A) r->plane[PLANE_A].color_mask = operand;
        break;
    case 0xC9:
        if (plane == PLANE_B) r->plane[PLANE_B].color_mask = operand;
        break;
    case 0xCA:
        if (plane == PLANE_A) r->plane[PLANE_A].dyuv_initial = operand;
        break;
    case 0xCB:
        if (plane == PLANE_B) r->plane[PLANE_B].dyuv_initial = operand;
        break;
    case 0xCD:
        if (plane == PLANE_A) {
            r->cursor.x = (uint16_t)(instruction & 0x3FFu);
            r->cursor.y = (uint16_t)((instruction >> 12) & 0x3FFu);
        }
        break;
    case 0xCE:
        if (plane == PLANE_A) {
            r->cursor.color = (uint8_t)(instruction & 15u);
            r->cursor.enabled = (uint8_t)((instruction >> 23) & 1u);
            r->cursor.complementary_blink = (uint8_t)((instruction >> 22) & 1u);
            r->cursor.on_units = (uint8_t)((instruction >> 19) & 7u);
            r->cursor.off_units = (uint8_t)((instruction >> 16) & 7u);
            r->cursor.high_resolution = (uint8_t)((instruction >> 15) & 1u);
            if (!r->cursor.on_units && r->cursor.off_units)
                video_error("zero cursor-on period with blinking enabled", instruction);
            r->cursor.start_field =
                atomic_load_explicit(&video.generation, memory_order_relaxed);
        }
        break;
    case 0xCF:
        if (plane == PLANE_A)
            r->cursor.pattern[(instruction >> 16) & 15u] = (uint16_t)instruction;
        break;
    case 0xD8:
        if (plane == PLANE_A) r->backdrop = (uint8_t)(instruction & 15u);
        break;
    case 0xD9:
    case 0xDA:
        if ((opcode == 0xD9 && plane == PLANE_A) ||
            (opcode == 0xDA && plane == PLANE_B)) {
            r->plane[plane].hold_enabled = (uint8_t)((instruction >> 23) & 1u);
            r->plane[plane].hold_count = (uint8_t)instruction;
        }
        break;
    case 0xDB:
        if (plane == PLANE_A) r->plane[PLANE_A].contribution =
            (uint8_t)(instruction & 63u);
        break;
    case 0xDC:
        if (plane == PLANE_B) r->plane[PLANE_B].contribution =
            (uint8_t)(instruction & 63u);
        break;
    default:
        break; /* reserved control words are no-ops */
    }
}

void mcd212_video_begin_frame(uint16_t base_width, uint16_t base_height,
                              int interlaced) {
    uint32_t width = (uint32_t)base_width * 2u;
    uint32_t height = interlaced ? (uint32_t)base_height * 2u : base_height;
    if (width > MCD212_VIDEO_MAX_WIDTH || height > MCD212_VIDEO_MAX_HEIGHT)
        video_error("invalid display geometry", (width << 16) | height);
#if CDI_VIDEO_HLE
    if (width != video.width || height != video.height)
        memset(completed_lines, 0, sizeof completed_lines);
#endif
    video.width = (uint16_t)width;
    video.height = (uint16_t)height;
}

static uint32_t clut_lookup(int plane, unsigned index) {
    unsigned bank_start;
    if (plane == PLANE_B) bank_start = 128;
    else if (video.registers.plane[PLANE_A].coding == CODING_CLUT77 &&
             video.registers.plane_a_high_clut) bank_start = 128;
    else bank_start = 0;
    return video.registers.clut[(bank_start + index) & 255u];
}

static uint16_t decode_clut(int plane, uint32_t address, uint16_t pixels,
                            int double_pixels) {
    PlaneSetup *setup = &video.registers.plane[plane];
    uint32_t *output = video.decoded[plane];
    unsigned out = 0;
    unsigned i;

    if (setup->coding == CODING_CLUT4) {
        unsigned byte_count = (pixels + 1u) / 2u;
        for (i = 0; i < byte_count; i++) {
            uint8_t packed = dram_byte(address + i);
            output[out++] = clut_lookup(plane, packed >> 4);
            if (out < pixels) output[out++] = clut_lookup(plane, packed & 15u);
        }
        return (uint16_t)byte_count;
    }

    {
        unsigned mask = setup->coding == CODING_CLUT8 ? 0xFFu : 0x7Fu;
        for (i = 0; i < pixels; i++) {
            uint32_t color = clut_lookup(plane, dram_byte(address + i) & mask);
            output[out++] = color;
            if (double_pixels) output[out++] = color;
        }
    }
    return pixels;
}

static uint16_t decode_rgb555(uint32_t address_a, uint32_t address_b,
                              uint16_t pixels) {
    uint32_t *output = video.decoded[PLANE_B];
    unsigned i;
    for (i = 0; i < pixels; i++) {
        uint16_t packed = (uint16_t)(((uint16_t)dram_byte(address_a + i) << 8) |
                                     dram_byte(address_b + i));
        uint32_t color = ((packed & 0x8000u) ? 0x80000000u : 0u) |
                         ((uint32_t)((packed >> 7) & 0xF8u) << 16) |
                         ((uint32_t)((packed >> 2) & 0xF8u) << 8) |
                         ((uint32_t)(packed << 3) & 0xF8u);
        output[i * 2u] = color;
        output[i * 2u + 1u] = color;
    }
    return pixels;
}

static uint32_t dyuv_rgb(uint8_t y, uint8_t u, uint8_t v) {
    return argb(saturate((int)y + 351 * ((int)v - 128) / 256),
                saturate((int)y - 86 * ((int)u - 128) / 256 -
                         179 * ((int)v - 128) / 256),
                saturate((int)y + 444 * ((int)u - 128) / 256));
}

static void put_decoded(uint32_t *output, unsigned *position,
                        uint32_t color, int duplicate) {
    output[(*position)++] = color;
    if (duplicate) output[(*position)++] = color;
}

static uint16_t decode_dyuv(int plane, uint32_t address, uint16_t pixels,
                            int duplicate) {
    static const uint8_t delta[16] = {
        0x00, 0x01, 0x04, 0x09, 0x10, 0x1B, 0x2C, 0x4F,
        0x80, 0xB1, 0xD4, 0xE5, 0xF0, 0xF7, 0xFC, 0xFF
    };
    uint32_t *output = video.decoded[plane];
    uint32_t initial = video.registers.plane[plane].dyuv_initial;
    uint8_t y = (uint8_t)(initial >> 16);
    uint8_t u = (uint8_t)(initial >> 8);
    uint8_t v = (uint8_t)initial;
    unsigned out = 0;
    unsigned i;

    for (i = 0; i < pixels; i += 2) {
        uint8_t first = dram_byte(address + i);
        uint8_t second = dram_byte(address + i + 1);
        int delta_u = (int8_t)delta[first >> 4];
        int delta_v = (int8_t)delta[second >> 4];
        int half_u = delta_u >= 0 ? delta_u / 2 : -((-delta_u + 1) / 2);
        int half_v = delta_v >= 0 ? delta_v / 2 : -((-delta_v + 1) / 2);
        uint8_t next_u = (uint8_t)(u + delta_u);
        uint8_t next_v = (uint8_t)(v + delta_v);
        /* Chroma interpolation follows the signed delta across the 8-bit
         * wrap point. Averaging the wrapped endpoints turns, for example,
         * 250 -> 10 into 130 rather than the intended midpoint 2. */
        uint8_t middle_u = (uint8_t)(u + half_u);
        uint8_t middle_v = (uint8_t)(v + half_v);
        uint8_t first_y = (uint8_t)(y + delta[first & 15u]);
        uint8_t second_y = (uint8_t)(first_y + delta[second & 15u]);

        put_decoded(output, &out, dyuv_rgb(first_y, middle_u, middle_v), duplicate);
        if (i + 1 < pixels)
            put_decoded(output, &out, dyuv_rgb(second_y, next_u, next_v), duplicate);
        y = second_y;
        u = next_u;
        v = next_v;
    }
    return (uint16_t)((pixels + 1u) & ~1u);
}

static uint16_t decode_run_length(int plane, uint32_t address,
                                  uint16_t output_pixels) {
    PlaneSetup *setup = &video.registers.plane[plane];
    uint32_t *output = video.decoded[plane];
    unsigned consumed = 0;
    unsigned x = 0;
    int paired = setup->bits_per_pixel == BITS_DOUBLE_4;
    int duplicate = !paired && output_pixels <= 384;

    while (x < output_pixels) {
        uint8_t format = dram_byte(address + consumed++);
        if (paired) {
            unsigned repetitions = 1;
            uint32_t first;
            uint32_t second;
            if (format & 0x80u) {
                repetitions = dram_byte(address + consumed++);
                if (!repetitions) repetitions = (output_pixels - x) / 2u;
            }
            first = clut_lookup(plane, (format >> 4) & 7u);
            second = clut_lookup(plane, format & 7u);
            while (repetitions-- && x + 1 < output_pixels) {
                output[x++] = first;
                output[x++] = second;
            }
        } else {
            unsigned repetitions = 1;
            uint32_t color;
            if (format & 0x80u) {
                repetitions = dram_byte(address + consumed++);
                if (!repetitions) repetitions = output_pixels - x;
            }
            color = clut_lookup(plane, format & 0x7Fu);
            while (repetitions-- && x < output_pixels) {
                if (duplicate) {
                    output[x * 2u] = color;
                    output[x * 2u + 1u] = color;
                } else {
                    output[x] = color;
                }
                x++;
            }
        }
    }
    return (uint16_t)consumed;
}

static uint16_t decode_plane(int plane, uint32_t address, uint32_t address_a,
                             uint16_t source_width) {
    PlaneSetup *setup = &video.registers.plane[plane];
    unsigned mosaic;
    unsigned original_width;
    int duplicate;
    uint16_t consumed;

    memset(video.decoded[plane], 0, sizeof video.decoded[plane]);
    if (setup->coding == CODING_OFF) return 0;

    if (((setup->file_type == FILE_BITMAP || setup->file_type == FILE_MOSAIC) &&
         setup->coding == CODING_CLUT4) ||
        (setup->file_type == FILE_RUN_LENGTH &&
         (setup->bits_per_pixel == BITS_DOUBLE_4 ||
          setup->bits_per_pixel == BITS_HIGH_8)))
        source_width = video.width;

    if (setup->file_type == FILE_RUN_LENGTH)
        return decode_run_length(plane, address, source_width);

    mosaic = setup->file_type == FILE_MOSAIC ? setup->mosaic_repeat : 1u;
    original_width = source_width;
    duplicate = setup->coding != CODING_CLUT4 && original_width <= 384u;
    if (mosaic > 1)
        source_width = (uint16_t)((original_width + mosaic - 1u) / mosaic);

    if (setup->coding == CODING_RGB555)
        consumed = decode_rgb555(address_a, address, source_width);
    else if (setup->coding == CODING_DYUV)
        consumed = decode_dyuv(plane, address, source_width, duplicate);
    else
        consumed = decode_clut(plane, address, source_width, duplicate);

    if (mosaic > 1) {
        uint32_t *output = video.decoded[plane];
        unsigned x = video.width;
        while (x--) output[x] = output[x / mosaic];
    }
    return consumed;
}

static void apply_pixel_hold(int plane) {
    PlaneSetup *setup = &video.registers.plane[plane];
    uint32_t *pixels = video.decoded[plane];
    unsigned start;
    if (!setup->hold_enabled) return;
    if (!setup->hold_count) video_error("reserved zero pixel-hold count", plane);
    for (start = 0; start < video.width; start += setup->hold_count) {
        unsigned end = start + setup->hold_count;
        unsigned x;
        if (end > video.width) end = video.width;
        for (x = start + 1; x < end; x++) pixels[x] = pixels[start];
    }
}

static uint32_t apply_contribution(uint32_t pixel, unsigned factor) {
    const uint8_t *table = video.contribution_lut[factor & 63u];
    return (pixel & 0xFF000000u) |
           ((uint32_t)table[(pixel >> 16) & 255u] << 16) |
           ((uint32_t)table[(pixel >> 8) & 255u] << 8) |
           table[pixel & 255u];
}

static int matches_color_key(int plane, uint32_t pixel) {
    PlaneSetup *setup = &video.registers.plane[plane];
    return (((pixel | setup->color_mask) & 0x00FCFCFCu) ==
            ((setup->transparent_color | setup->color_mask) & 0x00FCFCFCu));
}

static int transparent_pixel(int plane, uint32_t pixel, const uint8_t matte[2]) {
    uint8_t control = video.registers.plane[plane].transparency;
    int predicate;
    switch (control & 7u) {
    case 0: predicate = 1; break;
    case 1: predicate = matches_color_key(plane, pixel); break;
    case 2: predicate = (int)((pixel >> 31) & 1u); break;
    case 3: predicate = matte[0]; break;
    case 4: predicate = matte[1]; break;
    case 5: predicate = matte[0] || matches_color_key(plane, pixel); break;
    case 6: predicate = matte[1] || matches_color_key(plane, pixel); break;
    default: predicate = 0; break;
    }
    return predicate == !(control & 8u);
}

static uint32_t add_planes(uint32_t a, uint32_t b) {
    return argb(saturate((int)((a >> 16) & 255u) + (int)((b >> 16) & 255u) - 16),
                saturate((int)((a >> 8) & 255u) + (int)((b >> 8) & 255u) - 16),
                saturate((int)(a & 255u) + (int)(b & 255u) - 16));
}

typedef struct {
    unsigned next[2];
    unsigned limit[2];
    uint8_t flag[2];
    uint8_t factor[2];
} MatteState;

static void initialize_matte_state(MatteState *state) {
    state->next[0] = 0;
    state->next[1] = video.registers.two_mattes ? 4u : 8u;
    state->limit[0] = video.registers.two_mattes ? 4u : 8u;
    state->limit[1] = 8u;
    state->flag[0] = state->flag[1] = 0;
    state->factor[0] = video.registers.plane[0].contribution;
    state->factor[1] = video.registers.plane[1].contribution;
    if (!video.registers.two_mattes) {
        unsigned selected = (video.registers.matte_command[0] >> 16) & 1u;
        state->next[selected] = 0;
        state->limit[selected] = 8;
        state->next[!selected] = state->limit[!selected] = 8;
    }
}

static void apply_matte_commands(MatteState *state, unsigned x) {
    unsigned matte;
    for (matte = 0; matte < 2; matte++) {
        while (state->next[matte] < state->limit[matte]) {
            uint32_t command = video.registers.matte_command[state->next[matte]];
            unsigned operation;
            uint8_t value;
            if ((command & 0x3FFu) > x) break;
            operation = (command >> 20) & 15u;
            value = (uint8_t)((command >> 10) & 63u);
            if (operation == 0) {
                state->next[matte] = state->limit[matte];
                break;
            }
            if (operation == 4 || operation == 12 || operation == 13)
                state->factor[0] = value;
            if (operation == 6 || operation == 14 || operation == 15)
                state->factor[1] = value;
            if (operation == 8 || operation == 12 || operation == 14)
                state->flag[matte] = 0;
            if (operation == 9 || operation == 13 || operation == 15)
                state->flag[matte] = 1;
            state->next[matte]++;
        }
    }
}

static void draw_cursor(uint32_t *line_pixels, uint16_t line) {
    CursorSetup *cursor = &video.registers.cursor;
    uint32_t color;
    unsigned row;
    unsigned scale;
    uint16_t pattern;

    if (!cursor->enabled || line < cursor->y || line >= cursor->y + 16u) return;
    color = yrgb_color(cursor->color);
    if (cursor->off_units) {
        uint64_t field = atomic_load_explicit(&video.generation, memory_order_relaxed);
        uint64_t on_fields = 12u * cursor->on_units;
        uint64_t off_fields = 12u * cursor->off_units;
        uint64_t phase = (field - cursor->start_field) % (on_fields + off_fields);
        if (phase >= on_fields) {
            if (!cursor->complementary_blink) return;
            color ^= 0x00FFFFFFu;
        }
    }

    row = line - cursor->y;
    pattern = cursor->pattern[row];
    scale = cursor->high_resolution ? 1u : 2u;
    for (row = 0; row < 16; row++) {
        unsigned first_x;
        unsigned repeat;
        if (!(pattern & (0x8000u >> row))) continue;
        first_x = cursor->x + row * scale;
        for (repeat = 0; repeat < scale && first_x + repeat < video.width; repeat++)
            line_pixels[first_x + repeat] = color;
    }
}

static void compose_line(uint16_t line) {
    VideoRegisters *r = &video.registers;
    MatteState matte;
    uint32_t *destination;
    uint32_t backdrop;
    unsigned x;

    uint32_t *snap_a;
    uint32_t *snap_b;
    uint8_t *verdict;

    if (line >= video.height) return;
    destination = &video.frame[video.drawing_buffer][(uint32_t)line * video.width];
    snap_a = &video.plane_frame[PLANE_A][(uint32_t)line * video.width];
    snap_b = &video.plane_frame[PLANE_B][(uint32_t)line * video.width];
    verdict = &video.verdict_frame[(uint32_t)line * video.width];
    backdrop = yrgb_color(r->backdrop);
    initialize_matte_state(&matte);

    for (x = 0; x < video.width; x++) {
        uint32_t raw_a = video.decoded[PLANE_A][x];
        uint32_t raw_b = video.decoded[PLANE_B][x];
        uint32_t a;
        uint32_t b;
        int transparent_a;
        int transparent_b;
        int front_transparent;
        int back_transparent;
        uint32_t front;
        uint32_t back;

        apply_matte_commands(&matte, x);
        a = apply_contribution(raw_a, matte.factor[0]);
        b = apply_contribution(raw_b, matte.factor[1]);
        transparent_a = transparent_pixel(PLANE_A, raw_a, matte.flag);
        transparent_b = transparent_pixel(PLANE_B, raw_b, matte.flag);

        front = r->plane_b_front ? b : a;
        back = r->plane_b_front ? a : b;
        front_transparent = r->plane_b_front ? transparent_b : transparent_a;
        back_transparent = r->plane_b_front ? transparent_a : transparent_b;

        snap_a[x] = raw_a | 0xFF000000u;
        snap_b[x] = raw_b | 0xFF000000u;
        verdict[x] = (uint8_t)((transparent_a ? 1u : 0u) |
                               (transparent_b ? 2u : 0u));
        if (r->mixing && !transparent_a && !transparent_b) {
            destination[x] = add_planes(a, b);
            verdict[x] |= 0x20u;
        } else if (!front_transparent) {
            destination[x] = front | 0xFF000000u;
            verdict[x] |= 0x04u;
        } else if (!back_transparent) {
            destination[x] = back | 0xFF000000u;
            verdict[x] |= 0x08u;
        } else {
            destination[x] = backdrop;
            verdict[x] |= 0x10u;
        }
    }
    draw_cursor(destination, line);
}

#if CDI_VIDEO_HLE
/* A scanout job owns source consumption through final pixels and diagnostic
 * snapshots. Its boundaries are changes in source color or matte state, not
 * the decoder/contribution/transparency/compositor helper boundaries. RLE7
 * streams feed it directly; other formats retain their faithful decoders.
 * There is no cached guest memory or shadow register state between jobs. */
typedef struct {
    uint32_t address;
    uint32_t color;
    unsigned end;
    unsigned pixels;
    unsigned produced;
    unsigned scale;
    uint16_t consumed;
    int direct;
} ScanoutSource;

static ScanoutSource scanout_source(int plane, uint32_t address,
                                     uint32_t address_a, uint16_t width) {
    PlaneSetup *setup = &video.registers.plane[plane];
    ScanoutSource source = {0};
    uint16_t source_width = width;
    source.address = address;
    if (setup->file_type == FILE_RUN_LENGTH &&
        setup->bits_per_pixel == BITS_HIGH_8) {
        width = video.width;
    }
    source.scale = width <= 384u ? 2u : 1u;
    source.pixels = width;
    source.direct = !setup->hold_enabled &&
        setup->coding >= CODING_CLUT8 &&
        setup->file_type == FILE_RUN_LENGTH &&
        setup->bits_per_pixel != BITS_DOUBLE_4 &&
        (unsigned)width * source.scale == video.width;
    if (source.direct) {
        /* Match the reference's cleared, unused decoded-row tail. */
        memset(video.decoded[plane] + video.width, 0,
               (MCD212_VIDEO_MAX_WIDTH - video.width) * sizeof(uint32_t));
    } else {
        source.consumed = decode_plane(plane, address, address_a, source_width);
        apply_pixel_hold(plane);
    }
    return source;
}

static void scanout_next(int plane, ScanoutSource *source, unsigned x) {
    if (x < source->end) return;
    if (source->direct) {
        uint8_t format = dram_byte(source->address + source->consumed++);
        unsigned count = 1;
        unsigned remaining = source->pixels - source->produced;
        if (format & 0x80u) {
            count = dram_byte(source->address + source->consumed++);
            if (!count) count = remaining;
        }
        if (count > remaining) count = remaining;
        source->produced += count;
        source->end = source->produced * source->scale;
        source->color = clut_lookup(plane, format & 0x7Fu);
    } else {
        const uint32_t *row = video.decoded[plane];
        source->color = row[x];
        source->end = x + 1;
        while (source->end < video.width &&
               row[source->end] == source->color) source->end++;
    }
}

static unsigned scanout_matte_end(const MatteState *matte) {
    unsigned end = video.width;
    unsigned plane;
    for (plane = 0; plane < PLANE_COUNT; plane++) {
        if (matte->next[plane] < matte->limit[plane]) {
            unsigned next = video.registers.matte_command[matte->next[plane]] & 0x3FFu;
            if (next < end) end = next;
        }
    }
    return end;
}

static void scanout_span(uint16_t line, unsigned x, unsigned end,
                          uint32_t raw_a, uint32_t raw_b,
                          const MatteState *matte) {
    const VideoRegisters *r = &video.registers;
    uint32_t a = apply_contribution(raw_a, matte->factor[0]);
    uint32_t b = apply_contribution(raw_b, matte->factor[1]);
    int transparent_a = transparent_pixel(PLANE_A, raw_a, matte->flag);
    int transparent_b = transparent_pixel(PLANE_B, raw_b, matte->flag);
    int front_transparent = r->plane_b_front ? transparent_b : transparent_a;
    int back_transparent = r->plane_b_front ? transparent_a : transparent_b;
    uint8_t verdict = (uint8_t)((transparent_a ? 1u : 0u) |
                                (transparent_b ? 2u : 0u));
    uint32_t pixel;
    size_t offset = (size_t)line * video.width;
    uint32_t *destination = video.frame[video.drawing_buffer] + offset;
    uint32_t *snap_a = video.plane_frame[PLANE_A] + offset;
    uint32_t *snap_b = video.plane_frame[PLANE_B] + offset;
    uint8_t *verdicts = video.verdict_frame + offset;
    if (r->mixing && !transparent_a && !transparent_b) {
        pixel = add_planes(a, b);
        verdict |= 0x20u;
    } else if (!front_transparent) {
        pixel = (r->plane_b_front ? b : a) | 0xFF000000u;
        verdict |= 0x04u;
    } else if (!back_transparent) {
        pixel = (r->plane_b_front ? a : b) | 0xFF000000u;
        verdict |= 0x08u;
    } else {
        pixel = yrgb_color(r->backdrop);
        verdict |= 0x10u;
    }
    for (; x < end; x++) {
        destination[x] = pixel;
        snap_a[x] = raw_a | 0xFF000000u;
        snap_b[x] = raw_b | 0xFF000000u;
        verdicts[x] = verdict;
    }
}

static void scanout_job(uint32_t address_a, uint32_t address_b,
                         uint16_t line, uint16_t width_a, uint16_t width_b,
                         uint16_t *bytes_a, uint16_t *bytes_b) {
    ScanoutSource a = scanout_source(PLANE_A, address_a, address_a, width_a);
    ScanoutSource b = scanout_source(PLANE_B, address_b, address_a, width_b);
    MatteState matte;
    unsigned x = 0;
    initialize_matte_state(&matte);
    if (a.direct || b.direct) video.fused_jobs++;
    while (x < video.width) {
        unsigned end;
        unsigned i;
        scanout_next(PLANE_A, &a, x);
        scanout_next(PLANE_B, &b, x);
        apply_matte_commands(&matte, x);
        end = a.end < b.end ? a.end : b.end;
        if (scanout_matte_end(&matte) < end) end = scanout_matte_end(&matte);
        /* Preserve the same decoded-row diagnostics without a separate
         * expand/hold/composite pass over the direct source. */
        if (a.direct)
            for (i = x; i < end; i++) video.decoded[PLANE_A][i] = a.color;
        if (b.direct)
            for (i = x; i < end; i++) video.decoded[PLANE_B][i] = b.color;
        scanout_span(line, x, end, a.color, b.color, &matte);
        x = end;
    }
    draw_cursor(video.frame[video.drawing_buffer] + (size_t)line * video.width, line);
    if (bytes_a) *bytes_a = video.registers.plane[PLANE_B].coding == CODING_RGB555
                               ? b.consumed : a.consumed;
    if (bytes_b) *bytes_b = b.consumed;
}

static const uint8_t *line_source(uint32_t address, uint16_t count) {
    extern uint8_t g_ram0[CDI_RAM0_SIZE];
    extern uint8_t g_ram1[CDI_RAM1_SIZE];
    if (!count) return g_ram0;
    if (count > LINE_SOURCE_CAPACITY) return NULL;
    if (address < CDI_RAM0_SIZE && count <= CDI_RAM0_SIZE - address)
        return g_ram0 + address;
    if (address >= CDI_RAM1_BASE && address - CDI_RAM1_BASE < CDI_RAM1_SIZE &&
        count <= CDI_RAM1_SIZE - (address - CDI_RAM1_BASE))
        return g_ram1 + address - CDI_RAM1_BASE;
    return NULL;
}

static void reusable_line(uint32_t a, uint32_t b, uint16_t line,
                         uint16_t wa, uint16_t wb,
                         uint16_t *used_a, uint16_t *used_b) {
    CompletedLine *job = &completed_lines[line];
    VideoRegisters key = video.registers;
    const uint8_t *source[PLANE_COUNT];
    const CursorSetup *cursor = &video.registers.cursor;
    unsigned plane;
    /* RGB555 couples the two sources and may read more A bytes than its
     * published consumption. Keep the existing job until that input contract
     * is separately captured. Blinking cursor rows depend on field phase. */
    int eligible = video.registers.plane[PLANE_B].coding != CODING_RGB555 &&
        !(cursor->enabled && cursor->off_units && line >= cursor->y &&
          line < cursor->y + 16u);
    /* The restart field has no output effect without blinking. Control lists
     * commonly rewrite CE every field, even for a disabled cursor. */
    if (!cursor->enabled || !cursor->off_units) key.cursor.start_field = 0;
    if (eligible && job->valid && job->address[0] == a && job->address[1] == b &&
        job->width[0] == wa && job->width[1] == wb &&
        !memcmp(&job->registers, &key, sizeof job->registers)) {
        for (plane = 0; plane < PLANE_COUNT; plane++) {
            source[plane] = line_source(job->address[plane], job->consumed[plane]);
            if (!source[plane] || memcmp(source[plane], job->source[plane],
                                        job->consumed[plane])) break;
        }
        if (plane == PLANE_COUNT) {
            memcpy(video.decoded, job->decoded, sizeof video.decoded);
            if (used_a) *used_a = job->consumed[0];
            if (used_b) *used_b = job->consumed[1];
            video.reused_jobs++;
            return;
        }
    }
    job->valid = 0;
    scanout_job(a, b, line, wa, wb, &job->consumed[0], &job->consumed[1]);
    if (used_a) *used_a = job->consumed[0];
    if (used_b) *used_b = job->consumed[1];
    if (!eligible) return;
    job->address[0] = a; job->address[1] = b;
    job->width[0] = wa; job->width[1] = wb;
    for (plane = 0; plane < PLANE_COUNT; plane++) {
        source[plane] = line_source(job->address[plane], job->consumed[plane]);
        if (!source[plane]) return;
        memcpy(job->source[plane], source[plane], job->consumed[plane]);
    }
    job->registers = key;
    memcpy(job->decoded, video.decoded, sizeof job->decoded);
    job->valid = 1;
}
#endif

void mcd212_video_draw_line(uint32_t address_a, uint32_t address_b,
                            uint16_t line, uint16_t source_width_a,
                            uint16_t source_width_b,
                            uint16_t *bytes_a, uint16_t *bytes_b) {
    video.scanout_jobs++;
#if CDI_VIDEO_HLE
    /* Out-of-range lines still have reference decode/consumption semantics. */
    if (line < video.height) {
        reusable_line(address_a, address_b, line, source_width_a, source_width_b,
                      bytes_a, bytes_b);
        return;
    }
#endif
    uint16_t consumed_a = decode_plane(PLANE_A, address_a, address_a,
                                       source_width_a);
    uint16_t consumed_b = decode_plane(PLANE_B, address_b, address_a,
                                       source_width_b);
    if (video.registers.plane[PLANE_B].coding == CODING_RGB555)
        consumed_a = consumed_b;
    apply_pixel_hold(PLANE_A);
    apply_pixel_hold(PLANE_B);
    compose_line(line);
    if (bytes_a) *bytes_a = consumed_a;
    if (bytes_b) *bytes_b = consumed_b;
}

static uint64_t hash_frame_bytes(const uint32_t *pixels, size_t byte_count) {
    const uint8_t *bytes = (const uint8_t *)pixels;
    uint64_t hash = 0x14650FB0739D0383ULL;
    size_t i;
    for (i = 0; i < byte_count; i++) {
        hash ^= bytes[i];
        hash *= 0x00000100000001B3ULL;
    }
    return hash;
}

void mcd212_video_end_frame(void) {
    int completed = video.drawing_buffer;
#if CDI_VIDEO_HLE
    int previous = atomic_load_explicit(&video.published_buffer, memory_order_relaxed);
    size_t byte_count = (size_t)video.width * video.height * sizeof(uint32_t);
    int same_pixels = !memcmp(video.frame[previous], video.frame[completed], byte_count);
    int same_geometry = video.width == video.published_width &&
                        video.height == video.published_height;
    int initialized = atomic_load_explicit(&video.generation, memory_order_relaxed) != 0;
    int same_content = initialized && same_pixels && same_geometry;
    uint64_t hash = same_content
        ? atomic_load_explicit(&video.published_hash, memory_order_relaxed)
        : hash_frame_bytes(video.frame[completed], byte_count);
#endif
    atomic_fetch_add_explicit(&video.publish_sequence, 1, memory_order_acq_rel);
    atomic_store_explicit(&video.published_buffer, completed, memory_order_release);
    video.published_width = video.width;
    video.published_height = video.height;
#if CDI_VIDEO_HLE
    atomic_store_explicit(&video.published_hash, hash, memory_order_relaxed);
    if (!same_content)
        atomic_fetch_add_explicit(&video.content_generation, 1, memory_order_relaxed);
#else
    /* The reference republishes and presents every field, as before. */
    atomic_fetch_add_explicit(&video.content_generation, 1, memory_order_relaxed);
#endif
    atomic_fetch_add_explicit(&video.generation, 1, memory_order_release);
    atomic_fetch_add_explicit(&video.publish_sequence, 1, memory_order_release);
    video.drawing_buffer ^= 1;
    /* Undrawn scanlines retain the previous field's display memory. */
#if CDI_VIDEO_HLE
    /* Both full buffers were identical at the preceding field boundary.
     * Drawing writes only inside the current visible prefix. Copying that
     * changed prefix restores the full-buffer invariant, including hidden
     * storage that can reappear after a geometry change. */
    if (!same_pixels)
        memcpy(video.frame[video.drawing_buffer], video.frame[completed], byte_count);
#else
    memcpy(video.frame[video.drawing_buffer], video.frame[completed],
           sizeof video.frame[completed]);
#endif
}

uint32_t mcd212_video_copy_changed_frame(uint32_t *destination, uint32_t capacity,
                                         uint64_t previous_content,
                                         uint16_t *width, uint16_t *height,
                                         uint64_t *generation, uint64_t *content) {
    uint64_t before, after, field = 0, identity = 0;
    uint16_t w = 0, h = 0;
    uint32_t copied = 0;
    do {
        before = atomic_load_explicit(&video.publish_sequence, memory_order_acquire);
        if (before & 1u) { after = before + 1u; continue; }
        w = video.published_width;
        h = video.published_height;
        field = atomic_load_explicit(&video.generation, memory_order_acquire);
        identity = atomic_load_explicit(&video.content_generation, memory_order_relaxed);
        copied = field && identity != previous_content ? (uint32_t)w * h : 0;
        if (destination && copied) {
            int front = atomic_load_explicit(&video.published_buffer, memory_order_acquire);
            if (capacity < copied) video_error("framebuffer destination too small", capacity);
            memcpy(destination, video.frame[front], (size_t)copied * sizeof(uint32_t));
        }
        after = atomic_load_explicit(&video.publish_sequence, memory_order_acquire);
    } while (before != after || (after & 1u));
    if (width) *width = w;
    if (height) *height = h;
    if (generation) *generation = field;
    if (content) *content = identity;
    return copied;
}

uint32_t mcd212_video_copy_frame(uint32_t *destination, uint32_t capacity,
                                 uint16_t *width, uint16_t *height,
                                 uint64_t *generation) {
    uint64_t before;
    uint64_t after;
    uint64_t completed_generation = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    uint32_t pixels = 0;
    do {
        before = atomic_load_explicit(&video.publish_sequence, memory_order_acquire);
        if (before & 1u) {
            after = before + 1u;
            continue;
        }
        w = video.published_width;
        h = video.published_height;
        pixels = (uint32_t)w * h;
        if (destination) {
            int front;
            if (capacity < pixels) video_error("framebuffer destination too small", capacity);
            front = atomic_load_explicit(&video.published_buffer, memory_order_acquire);
            memcpy(destination, video.frame[front], (size_t)pixels * sizeof(uint32_t));
        }
        completed_generation =
            atomic_load_explicit(&video.generation, memory_order_acquire);
        after = atomic_load_explicit(&video.publish_sequence, memory_order_acquire);
    } while (before != after || (after & 1u));
    if (width) *width = w;
    if (height) *height = h;
    if (generation) *generation = completed_generation;
    return pixels;
}

uint64_t mcd212_video_frame_hash(uint16_t *width, uint16_t *height,
                                 uint64_t *generation) {
    uint64_t before;
    uint64_t after;
    uint64_t completed_generation = 0;
    uint64_t hash = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    do {
        size_t byte_count;
        int front;
        const uint8_t *bytes;
        before = atomic_load_explicit(&video.publish_sequence, memory_order_acquire);
        if (before & 1u) {
            after = before + 1u;
            continue;
        }
        w = video.published_width;
        h = video.published_height;
        front = atomic_load_explicit(&video.published_buffer, memory_order_acquire);
        bytes = (const uint8_t *)video.frame[front];
        byte_count = (size_t)w * h * sizeof(uint32_t);
        completed_generation =
            atomic_load_explicit(&video.generation, memory_order_acquire);
#if CDI_VIDEO_HLE
        hash = completed_generation
            ? atomic_load_explicit(&video.published_hash, memory_order_relaxed)
            : hash_frame_bytes((const uint32_t *)bytes, byte_count);
#else
        hash = hash_frame_bytes((const uint32_t *)bytes, byte_count);
#endif
        after = atomic_load_explicit(&video.publish_sequence, memory_order_acquire);
    } while (before != after || (after & 1u));
    if (width) *width = w;
    if (height) *height = h;
    if (generation) *generation = completed_generation;
    return hash;
}

void mcd212_video_debug_cursor(Mcd212VideoCursorState *out) {
    CursorSetup *cursor = &video.registers.cursor;
    if (!out) return;
    out->x = cursor->x;
    out->y = cursor->y;
    memcpy(out->pattern, cursor->pattern, sizeof out->pattern);
    out->enabled = cursor->enabled;
    out->color = cursor->color;
    out->double_resolution = cursor->high_resolution;
    out->blink_type = cursor->complementary_blink;
    out->blink_on = cursor->on_units;
    out->blink_off = cursor->off_units;
}

void mcd212_video_debug_state(Mcd212VideoDebugState *out) {
    VideoRegisters *r = &video.registers;
    const uint8_t *clut_bytes = (const uint8_t *)r->clut;
    size_t i;
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->scanout_jobs = video.scanout_jobs;
    out->fused_jobs = video.fused_jobs;
    out->reused_jobs = video.reused_jobs;
    for (i = 0; i < PLANE_COUNT; i++) {
        size_t x;
        out->plane_line_hash[i] = 0x14650FB0739D0383ULL;
        out->coding[i] = r->plane[i].coding;
        out->image_type[i] = r->plane[i].file_type;
        out->bpp[i] = r->plane[i].bits_per_pixel;
        out->pixel_repeat[i] = r->plane[i].mosaic_repeat;
        out->transparency[i] = r->plane[i].transparency;
        out->icf[i] = r->plane[i].contribution;
        out->hold_enabled[i] = r->plane[i].hold_enabled;
        out->hold_factor[i] = r->plane[i].hold_count;
        out->dyuv_start[i] = r->plane[i].dyuv_initial;
        out->transparent_color[i] = r->plane[i].transparent_color;
        out->mask_color[i] = r->plane[i].color_mask;
        out->plane_line_first[i] = video.decoded[i][0];
        for (x = 0; x < video.width; x++) {
            uint32_t pixel = video.decoded[i][x];
            const uint8_t *bytes = (const uint8_t *)&pixel;
            size_t byte;
            if (pixel & 0x00FFFFFFu) out->plane_line_nonblack[i]++;
            for (byte = 0; byte < sizeof pixel; byte++) {
                out->plane_line_hash[i] ^= bytes[byte];
                out->plane_line_hash[i] *= 0x00000100000001B3ULL;
            }
        }
    }
    out->clut_bank = r->clut_bank;
    out->backdrop = r->backdrop;
    out->plane_b_front = r->plane_b_front;
    out->mix = r->mixing;
    out->clut_select_high = r->plane_a_high_clut;
    out->two_mattes = r->two_mattes;
    out->external_video = r->external_video;
    out->clut_hash = 0x14650FB0739D0383ULL;
    for (i = 0; i < sizeof r->clut; i++) {
        out->clut_hash ^= clut_bytes[i];
        out->clut_hash *= 0x00000100000001B3ULL;
    }
}

void mcd212_video_debug_clut(uint32_t out[256]) {
    if (out) memcpy(out, video.registers.clut, sizeof video.registers.clut);
}

/* Debug: raw decoded plane rows and per-pixel composition verdicts, captured
 * always-on by compose_line (never armed).  plane 0/1 = decoded pixels;
 * verdict bits: 1 transparent_a, 2 transparent_b, 4 front shown, 8 back
 * shown, 16 backdrop shown, 32 mixed. */
unsigned mcd212_video_debug_plane_row(int plane, unsigned y,
                                      uint32_t *out, unsigned capacity) {
    unsigned width = video.width;
    if (!out || plane < 0 || plane >= (int)PLANE_COUNT) return 0;
    if (y >= video.height || width > capacity) return 0;
    memcpy(out, &video.plane_frame[plane][(size_t)y * width],
           (size_t)width * sizeof *out);
    return width;
}

unsigned mcd212_video_debug_verdict_row(unsigned y, uint8_t *out,
                                        unsigned capacity) {
    unsigned width = video.width;
    if (!out || y >= video.height || width > capacity) return 0;
    memcpy(out, &video.verdict_frame[(size_t)y * width], width);
    return width;
}
