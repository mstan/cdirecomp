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

/* Register byte-offsets within the $4FFFE0 window (CeDImu MCD212.hpp:
 * m_internalRegisters[addr - 0x4FFFE0]). DA frame geometry is read live from
 * these — the boot programs DCR1.FD/CF before the display polls matter, and
 * the oracle uses the live values, so mirroring them keeps us in phase. */
#define MCD_CSR1W 0x10u
#define MCD_DCR1  0x12u

/* ---- Display timing (CSR1R.DA, bit 7) — faithful port of CeDImu ----
 * The boot polls `BTST #7,$4FFFF1 / BNE` for DA (Display Active). CeDImu
 * (MCD212::IncrementTime + Display.cpp::DrawVideoLine) drives it from a
 * vertical line counter advanced by ELAPSED TIME, not raw cycles: each
 * instruction adds `cycles * cycleDelay` ns, and one display line elapses every
 * GetLineDisplayTime() ns. DA is clear through the vertical-retrace lines and
 * set during the active scan. We mirror that exactly — accumulating ns at the
 * SCC68070 cycle period and deriving the line time / frame geometry from the
 * live register file — so our DA phase tracks the oracle's (MC-CDI-005). The
 * cycle costs feeding this come from m68k_cycles (interpreter) and the
 * recompiled tier's accumulator; matching the oracle's *time base* here closes
 * the loop. NTSC board (the ROM under test); PAL would use 15.0 MHz / m_isPAL. */
#define MCD_DA  0x80u

/* SCC68070 NTSC clock: ns between cycles = 1e9 / 15,104,900 (SCC68070.hpp). */
#define MCD_CYCLE_DELAY_NS (1.0e9 / 15104900.0)

static double   s_time_ns;      /* ns accumulated toward the next line       */
static uint32_t s_vlines;       /* vertical line within the current frame    */

/* Live frame geometry (CeDImu MCD212.hpp GetFD/GetST/GetCF). FD selects NTSC
 * (262 lines / 22 retrace) vs the larger PAL-rate raster; CF shortens the line
 * display time. Read fresh each tick so we track register programming. */
static unsigned mcd_total_lines(void)   { return (s_reg[MCD_DCR1] & (1u<<13)) ? 262u : 312u; }
static unsigned mcd_retrace_lines(void) {
    if (s_reg[MCD_DCR1] & (1u<<13)) return 22u;             /* FD (NTSC)        */
    return (s_reg[MCD_CSR1W] & (1u<<1)) ? 72u : 32u;        /* !FD: ST selects  */
}
static double   mcd_line_time_ns(void)  { return (s_reg[MCD_DCR1] & (1u<<14)) ? 63560.0 : 64000.0; } /* CF */

void mcd212_tick(uint32_t cycles) {
    s_time_ns += (double)cycles * MCD_CYCLE_DELAY_NS;
    double line_ns = mcd_line_time_ns();
    while (s_time_ns >= line_ns) {
        s_time_ns -= line_ns;
        s_vlines++;
        if (s_vlines == mcd_retrace_lines() + 1)    /* leaving retrace -> active */
            s_csr1r |= MCD_DA;
        if (s_vlines >= mcd_total_lines()) {        /* frame end -> retrace      */
            s_csr1r &= (uint8_t)~MCD_DA;
            s_vlines = 0;
            g_frame_count++;
        }
    }
}

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
