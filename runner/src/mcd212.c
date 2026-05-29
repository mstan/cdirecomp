/*
 * mcd212.c — MCD212 Video & System Display Controller (skeleton).
 *
 * The MCD212 is the CD-i display heart: two independent image planes (A/B),
 * DYUV / RGB555 / CLUT4/7/8 / RL7 / mosaic coding, region-of-interest control,
 * and the system DRAM interface. It also drives the display-line interrupt
 * that paces a CD-i frame (PAL 384x280, ~50 Hz; NTSC 360x240).
 *
 * Reference implementation: external/CeDImu/src/CDI/cores/MCD212/. Port the
 * register file + DYUV/CLUT decoders from there (TODO MC-CDI-012). For now,
 * register access fails loud so the first real touch tells us exactly which
 * register Hotel Mario programs first.
 */
#include "cdi_runtime.h"
#include <stdio.h>
#include <stdlib.h>

uint32_t mcd212_read(uint32_t addr, int size) {
    fprintf(stderr, "[mcd212] read%d @ $%08X — register file not modelled "
                    "(TODO MC-CDI-012)\n", size * 8, addr);
    abort();
}
void mcd212_write(uint32_t addr, uint32_t val, int size) {
    fprintf(stderr, "[mcd212] write%d @ $%08X = $%X — register file not modelled "
                    "(TODO MC-CDI-012)\n", size * 8, addr, val);
    abort();
}
void mcd212_render_frame(uint32_t *framebuf) {
    (void)framebuf;
    /* TODO MC-CDI-012: composite plane A/B from DRAM into ARGB8888. */
}
