/*
 * mcd212_video.h - clean-room MCD212 pixel pipeline.
 *
 * The register/timing/control-area sequencer remains in mcd212.c.  This module
 * receives only decoded display-control instructions and DRAM addresses.  That
 * keeps the renderer deterministic and makes the eventual host frontend a
 * read-only consumer of completed ARGB frames.
 */
#pragma once

#include <stdint.h>

#define MCD212_VIDEO_MAX_WIDTH  768u
#define MCD212_VIDEO_MAX_HEIGHT 560u

void mcd212_video_reset(void);

/* Apply one non-flow-control ICA/DCA word to the persistent display state. */
void mcd212_video_control(int plane, uint32_t instruction);

/* Establish output geometry at the first active line of a frame. */
void mcd212_video_begin_frame(uint16_t base_width, uint16_t base_height,
                              int interlaced);

/* Decode and composite one active line. Returns bytes consumed by A and B. */
void mcd212_video_draw_line(uint32_t address_a, uint32_t address_b,
                            uint16_t line, uint16_t source_width_a,
                            uint16_t source_width_b,
                            uint16_t *bytes_a, uint16_t *bytes_b);

/* Publish the completed back buffer atomically at the frame boundary. */
void mcd212_video_end_frame(void);

/* Copy the last completed frame; returns the number of pixels copied. */
uint32_t mcd212_video_copy_frame(uint32_t *dst, uint32_t capacity,
                                 uint16_t *width, uint16_t *height,
                                 uint64_t *generation);
uint64_t mcd212_video_frame_hash(uint16_t *width, uint16_t *height,
                                 uint64_t *generation);

typedef struct {
    uint16_t x, y;
    uint16_t pattern[16];
    uint8_t enabled, color, double_resolution;
    uint8_t blink_type, blink_on, blink_off;
} Mcd212VideoCursorState;

typedef struct {
    uint8_t coding[2], image_type[2], bpp[2], pixel_repeat[2];
    uint8_t transparency[2], icf[2], hold_enabled[2], hold_factor[2];
    uint8_t clut_bank, backdrop, plane_b_front, mix;
    uint8_t clut_select_high, two_mattes, external_video;
    uint32_t dyuv_start[2], transparent_color[2], mask_color[2];
    uint32_t plane_line_first[2], plane_line_nonblack[2];
    uint64_t plane_line_hash[2];
    uint64_t clut_hash;
} Mcd212VideoDebugState;

/* Side-effect-free snapshot of the hardware-cursor control state.  The BIOS
 * updates this through ordinary ICA/DCA instructions; exposing it lets the
 * always-on debugger distinguish a blank cursor or blink from failed input. */
void mcd212_video_debug_cursor(Mcd212VideoCursorState *out);

/* Persistent control-program state used by the pixel decoder.  This is a
 * read-only diagnostic snapshot; it never changes display timing or state. */
void mcd212_video_debug_state(Mcd212VideoDebugState *out);
