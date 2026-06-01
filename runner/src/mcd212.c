/*
 * mcd212.c — MCD212 Video & System Display Controller (register file).
 *
 * The MCD212 is the CD-i display heart (two image planes, DYUV/RGB555/CLUT/RL7,
 * region control) and the system DRAM interface. In CeDImu the DRAM half also
 * serves system RAM; here RAM is handled by cdi_bus.c, so this models only the
 * memory-mapped register window at $4FFFE0..$4FFFFF (faithful port of CeDImu
 * MCD212 DRAMInterface.cpp):
 *   - writes go to the internal register file (CSR/DCR/DDR/VSR/DCP for planes
 *     1 & 2, byte-offset indexed; even byte = high, odd byte = low)
 *   - reads: only CSR1R ($4FFFF0/$4FFFF1) and CSR2R ($4FFFE0/$4FFFE1) are
 *     readable status registers; CSR2R clears (IT1/IT2/BE) on read; any other
 *     register read is a bus error on hardware (fail loud here).
 *
 * The pixel pipeline (DYUV/CLUT decode, plane A/B compositing) and the
 * display-line interrupt are deferred (TODO MC-CDI-012 / MC-CDI-007); the
 * register file is what early CD-RTOS boot programs.
 */
#include "cdi_runtime.h"
#include <stdio.h>
#include <stdlib.h>

#define MCD_BASE 0x004FFFE0u

static uint16_t s_reg[32];      /* internal registers, byte-offset indexed */
static uint8_t  s_csr1r = 0;    /* CSR1R read-side status (display)         */
static uint8_t  s_csr2r = 0;    /* CSR2R read-side status (IT1/IT2/BE)      */

uint32_t mcd212_read(uint32_t addr, int size) {
    if (addr == 0x004FFFF0u || addr == 0x004FFFF1u)        /* CSR1R (word LSB / byte) */
        return s_csr1r;
    if (addr == 0x004FFFE0u || addr == 0x004FFFE1u) {      /* CSR2R, clears on read   */
        uint8_t v = s_csr2r;
        s_csr2r = 0;
        return v;
    }
    fprintf(stderr, "[mcd212] read%d @ $%08X — only CSR1R/CSR2R are readable "
                    "(would bus-error on hardware) (TODO MC-CDI-012)\n", size * 8, addr);
    abort();
}

void mcd212_write(uint32_t addr, uint32_t val, int size) {
    uint32_t off = addr - MCD_BASE;
    if (off >= 32) {
        fprintf(stderr, "[mcd212] write%d @ $%08X = $%X out of register window\n",
                size * 8, addr, val);
        abort();
    }
    if (size >= 2) {
        s_reg[off] = (uint16_t)val;
    } else if ((addr & 1) == 0) {       /* even byte -> high byte of register */
        s_reg[off] = (uint16_t)((s_reg[off] & 0x00FF) | ((uint16_t)(val & 0xFF) << 8));
    } else {                            /* odd byte  -> low byte              */
        s_reg[off] = (uint16_t)((s_reg[off] & 0xFF00) | (val & 0xFF));
    }
    /* TODO MC-CDI-012: react to DCR.DE (display enable), ICA/DCA program
     * reloads, CLUT loads — drive the renderer + display-line interrupt. */
}

void mcd212_render_frame(uint32_t *framebuf) {
    (void)framebuf;
    /* TODO MC-CDI-012: composite plane A/B from DRAM into ARGB8888. */
}
