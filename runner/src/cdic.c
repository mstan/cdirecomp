/*
 * cdic.c — CDIC: CD Interface Controller (skeleton).
 *
 * The CDIC streams Mode-2 sectors off the disc, raises the data-ready
 * interrupt, and decodes CD-i ADPCM audio (levels A/B/C) into PCM. CD-RTOS
 * talks to it to read files/modules and to play the audio that is interleaved
 * with the level-data sectors (the L*_s*_sub.o streaming model we saw in the
 * disc inventory).
 *
 * Reference: external/CeDImu CDIC core + our own disc_parser sector model.
 * TODO MC-CDI-013: sector DMA + ADPCM decode + interrupts.
 */
#include "cdi_runtime.h"
#include <stdio.h>
#include <stdlib.h>

uint32_t cdic_read(uint32_t addr, int size) {
    fprintf(stderr, "[cdic] read%d @ $%08X — CD controller not modelled "
                    "(TODO MC-CDI-013)\n", size * 8, addr);
    abort();
}
void cdic_write(uint32_t addr, uint32_t val, int size) {
    fprintf(stderr, "[cdic] write%d @ $%08X = $%X — CD controller not modelled "
                    "(TODO MC-CDI-013)\n", size * 8, addr, val);
    abort();
}
