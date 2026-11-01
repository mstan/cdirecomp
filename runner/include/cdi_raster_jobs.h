#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include "mcd212_video.h"

/* Integer-authored visible spans, owned at their natural raster boundary.
 * mode0 copies A; mode1 applies contribution A; mode2 adds contributed A/B.
 * Hidden layers have no payload. Factor arithmetic is the native video rule. */
typedef struct {uint32_t bounds,a,b,control;} CdiRasterSpan;
extern int g_cdi_raster_capture_enabled;
typedef struct {
    atomic_uint references;
    uint32_t count;
    uint64_t identity;
    CdiRasterSpan spans[];
} CdiRasterRow;
typedef struct {
    atomic_uint references;
    uint16_t width,height;
    uint64_t generation,epoch;
    const CdiRasterRow *rows[MCD212_VIDEO_MAX_HEIGHT];
} CdiRasterFrame;

int cdi_raster_capture(int enabled);
int cdi_raster_capturing(void);
uint64_t cdi_raster_epoch(void);
void cdi_raster_reset(void);
void cdi_raster_begin(unsigned row,unsigned width,unsigned height);
void cdi_raster_span(unsigned first,unsigned last,uint32_t a,uint32_t b,uint32_t control);
void cdi_raster_overlay(unsigned first,unsigned last,uint32_t color);
void cdi_raster_finish(void);
void cdi_raster_publish(const uint32_t *native,unsigned width,unsigned height,uint64_t generation);
/* Acquire on the CPU publication thread. An acquired frame may be retained or
 * handed to another thread; no input mutates until its last owner releases it. */
const CdiRasterFrame *cdi_raster_acquire(void);
void cdi_raster_release(const CdiRasterFrame *);
uint32_t cdi_raster_color(const CdiRasterSpan *);
int cdi_raster_native(const CdiRasterFrame *,uint32_t *pixels,uint32_t capacity);
