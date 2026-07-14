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
 * Scan timing, ICA/DCA sequencing and pointer registers live here. The
 * deterministic pixel pipeline is isolated in mcd212_video.c.
 */
#include "cdi_runtime.h"
#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCD_BASE 0x004FFFE0u

static uint16_t s_reg[32];      /* internal registers, byte-offset indexed */
static uint8_t  s_csr1r = 0;    /* CSR1R read-side status (display)         */
static uint8_t  s_csr2r = 0;    /* CSR2R read-side status (IT1/IT2/BE)      */

/* Register byte-offsets within the $4FFFE0 window (CeDImu MCD212.hpp:
 * m_internalRegisters[addr - 0x4FFFE0]). DA frame geometry is read live from
 * these — the boot programs DCR1.FD/CF before the display polls matter, and
 * the oracle uses the live values, so mirroring them keeps us in phase. */
#define MCD_CSR1W 0x10u
#define MCD_DCR2  0x02u
#define MCD_VSR2  0x04u
#define MCD_DDR2  0x08u
#define MCD_DCP2  0x0Au
#define MCD_DCR1  0x12u
#define MCD_VSR1  0x14u
#define MCD_DDR1  0x18u
#define MCD_DCP1  0x1Au

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
/* CSR1R.PA (bit 5, Parity) — CeDImu Display.cpp::DrawVideoLine sets PA every
 * active line: non-interlaced -> always set; interlaced (DCR1.SM) -> set on odd
 * frames, clear on even. UnsetDA at frame end keeps PA (CeDImu `&= 0x20`), so
 * CSR1R reads 0x20 during retrace and 0xA0 during active display. The boot's
 * `MOVE.W $4FFFF0,Dn` polls both bits (DA+PA); modelling only DA gave 0x80 vs
 * the oracle's 0xA0 and diverged at $41BF94. */
#define MCD_PA  0x20u
#define MCD_DCR1_SM (1u<<12)   /* DCR1.SM: 1 = interlaced scan mode */

/* SCC68070 NTSC clock: ns between cycles = 1e9 / 15,104,900 (SCC68070.hpp). */
#define MCD_CYCLE_DELAY_NS (1.0e9 / 15104900.0)

static double   s_time_ns;      /* ns accumulated toward the next line       */
static uint32_t s_vlines;       /* vertical line within the current frame    */
static uint16_t s_line_number;  /* active output line (interlace advances 2) */

/* Live frame geometry (CeDImu MCD212.hpp GetFD/GetST/GetCF). FD selects NTSC
 * (262 lines / 22 retrace) vs the larger PAL-rate raster; CF shortens the line
 * display time. Read fresh each tick so we track register programming. */
static unsigned mcd_total_lines(void)   { return (s_reg[MCD_DCR1] & (1u<<13)) ? 262u : 312u; }
static unsigned mcd_retrace_lines(void) {
    if (s_reg[MCD_DCR1] & (1u<<13)) return 22u;             /* FD (NTSC)        */
    return (s_reg[MCD_CSR1W] & (1u<<1)) ? 72u : 32u;        /* !FD: ST selects  */
}
static double   mcd_line_time_ns(void)  { return (s_reg[MCD_DCR1] & (1u<<14)) ? 63560.0 : 64000.0; } /* CF */
static unsigned mcd_base_width(void) {
    /* Host raster format, not CM source-fetch width: the only 360-wide
     * monitor format is CF=0/ST=0. TV formats are 384 wide. */
    return !(s_reg[MCD_DCR1] & (1u << 14)) &&
           !(s_reg[MCD_CSR1W] & (1u << 1)) ? 360u : 384u;
}
static unsigned mcd_active_height(void) {
    if (s_reg[MCD_DCR1] & (1u << 13)) return 240u;
    return !(s_reg[MCD_DCR1] & (1u << 14)) &&
           !(s_reg[MCD_CSR1W] & (1u << 1)) ? 240u : 280u;
}

static uint8_t mcd_dram8(uint32_t address) {
    extern uint8_t g_ram0[CDI_RAM0_SIZE];
    extern uint8_t g_ram1[CDI_RAM1_SIZE];
    if (address < CDI_RAM0_SIZE) return g_ram0[address];
    if (address >= CDI_RAM1_BASE && address - CDI_RAM1_BASE < CDI_RAM1_SIZE)
        return g_ram1[address - CDI_RAM1_BASE];
    fprintf(stderr, "[mcd212] control fetch outside DRAM @ $%08X\n", address);
    abort();
}

static uint32_t mcd_control_word(uint32_t address) {
    return ((uint32_t)mcd_dram8(address) << 24) |
           ((uint32_t)mcd_dram8(address + 1) << 16) |
           ((uint32_t)mcd_dram8(address + 2) << 8) |
           mcd_dram8(address + 3);
}

static uint32_t mcd_get_pointer(unsigned high_reg, unsigned low_reg) {
    return ((uint32_t)(s_reg[high_reg] & 0x3Fu) << 16) | s_reg[low_reg];
}

static void mcd_set_vsr(int plane, uint32_t value) {
    unsigned dcr = plane ? MCD_DCR2 : MCD_DCR1;
    unsigned vsr = plane ? MCD_VSR2 : MCD_VSR1;
    s_reg[dcr] &= plane ? 0x0B00u : 0xFB00u;
    s_reg[dcr] |= (uint16_t)((value >> 16) & 0x3Fu);
    s_reg[vsr] = (uint16_t)value;
}

static void mcd_set_dcp(int plane, uint32_t value) {
    unsigned ddr = plane ? MCD_DDR2 : MCD_DDR1;
    unsigned dcp = plane ? MCD_DCP2 : MCD_DCP1;
    s_reg[ddr] &= 0x0F00u;
    s_reg[ddr] |= (uint16_t)((value >> 16) & 0x3Fu);
    s_reg[dcp] = (uint16_t)value;
}

static uint32_t mcd_get_vsr(int plane) {
    return plane ? mcd_get_pointer(MCD_DCR2, MCD_VSR2) :
                   mcd_get_pointer(MCD_DCR1, MCD_VSR1);
}

static uint32_t mcd_get_dcp(int plane) {
    return plane ? mcd_get_pointer(MCD_DDR2, MCD_DCP2) :
                   mcd_get_pointer(MCD_DDR1, MCD_DCP1);
}

static void mcd_interrupt(int plane) {
    uint8_t bit = plane ? 0x02u : 0x04u;
    uint16_t control = s_reg[plane ? 0x00u : MCD_CSR1W];
    s_csr2r = (uint8_t)((s_csr2r & (plane ? 0x05u : 0x03u)) | bit);
    if (!(control & 0x8000u)) cdi_irq_raise_onchip(1);
}

static int mcd_flow_instruction(int plane, uint32_t word, uint32_t *address,
                                int is_ica) {
    uint32_t pointer = word & 0x003FFFFCu;
    switch (word >> 28) {
    case 0: return 1;
    case 1: break;
    case 2: mcd_set_dcp(plane, pointer); break;
    case 3: mcd_set_dcp(plane, pointer); return 1;
    case 4:
        if (is_ica) *address = word & 0x003FFFFFu;
        else mcd_set_vsr(plane, word & 0x003FFFFFu);
        break;
    case 5: mcd_set_vsr(plane, word & 0x003FFFFFu); return 1;
    case 6: mcd_interrupt(plane); break;
    case 7:
        if ((word >> 24) == 0x78u) {
            unsigned dcr = plane ? MCD_DCR2 : MCD_DCR1;
            unsigned ddr = plane ? MCD_DDR2 : MCD_DDR1;
            uint16_t cm = (uint16_t)((word >> 4) & 1u);
            uint16_t mf = (uint16_t)((word >> 2) & 3u);
            uint16_t ft = (uint16_t)(word & 3u);
            s_reg[dcr] = (uint16_t)((s_reg[dcr] & (plane ? 0x033Fu : 0xF33Fu)) |
                                    (cm << 11));
            s_reg[ddr] = (uint16_t)((s_reg[ddr] & 0x003Fu) |
                                    (mf << 10) | (ft << 8));
        }
        break;
    default: break;
    }
    return 0;
}

static void mcd_execute_ica(int plane) {
    unsigned budget = (s_reg[MCD_DCR1] & (1u << 14) ? 120u : 112u) *
                      mcd_retrace_lines();
    uint32_t address = ((s_reg[MCD_DCR1] & MCD_DCR1_SM) && !(s_csr1r & MCD_PA)) ?
                       0x404u : 0x400u;
    if (plane) address += CDI_RAM1_BASE;
    for (unsigned i = 0; i < budget; i++) {
        uint32_t word = mcd_control_word(address);
        debug_trace_mcd_event(plane ? 2u : 0u, 0, word);
        address += 4;
        if (mcd_flow_instruction(plane, word, &address, 1)) return;
        mcd212_video_control(plane, word);
    }
}

static void mcd_execute_dca(int plane) {
    unsigned budget = s_reg[MCD_DCR1] & (1u << 14) ? 16u : 8u;
    for (unsigned i = 0; i < budget; i++) {
        uint32_t address = mcd_get_dcp(plane);
        mcd_set_dcp(plane, address + 4);
        uint32_t word = mcd_control_word(address);
        debug_trace_mcd_event(plane ? 3u : 1u, s_line_number, word);
        if (mcd_flow_instruction(plane, word, &address, 0)) return;
        mcd212_video_control(plane, word);
    }
}

void mcd212_tick(uint32_t cycles) {
    const double ns = (double)cycles * MCD_CYCLE_DELAY_NS;
    /* CeDImu advances the SCC68070 timer before CDI::IncrementTime on every
     * interpreter iteration, including the stopped-CPU path. */
    periph_increment_timer(cycles);
    /* Advance the IKAT's delayed-response check BEFORE the frame counter may
     * advance this tick. CeDImu's Mono3::IncrementTime runs CDI::IncrementTime
     * (→ IKAT::IncrementTime, which fires a 2-frame-delayed response the instant
     * GetTotalFrameCount() reaches its target) BEFORE m_mcd212.IncrementTime
     * (which increments the frame). Checking here, ahead of the g_frame_count++
     * below, fires the disc-status IRQ (set_int → cdi_irq_raise(2)) on the SAME
     * guest instruction as the oracle — checking after the increment would fire
     * one instruction early. mcd212_tick is called once per instruction in both
     * tiers, so this matches CeDImu's per-step IKAT advance. (MC-CDI-007.) */
    slave_increment_time(ns);
    /* CDI::IncrementTime (CDI.cpp:108-112) runs m_slave->IncrementTime(ns) then
     * m_timekeeper->IncrementClock(ns) BEFORE Mono3::IncrementTime's own
     * m_mcd212.IncrementTime(ns) — tick the DS1216 here, same ns, same order,
     * so its internal clock advances on the identical per-instruction schedule
     * as the oracle's (MC-CDI-022; see cdi_nvram.c nvram_increment_clock). */
    nvram_increment_clock(ns);
    g_total_cycles += cycles;   /* running SCC68070 clock; diffed per seq vs CeDImu totalCycleCount */
    s_time_ns += ns;
    double line_ns = mcd_line_time_ns();
    while (s_time_ns >= line_ns) {
        s_time_ns -= line_ns;
        s_vlines++;
        if (s_vlines <= mcd_retrace_lines()) {
            if (s_vlines == 1 && (s_reg[MCD_DCR1] & (1u << 15))) {
                if (s_reg[MCD_DCR1] & (1u << 9)) mcd_execute_ica(0);
                if (s_reg[MCD_DCR2] & (1u << 9)) mcd_execute_ica(1);
            }
            continue;
        }

        s_csr1r |= MCD_DA;
        if (s_reg[MCD_DCR1] & MCD_DCR1_SM) {
            if (!(g_frame_count & 1)) {
                s_csr1r &= (uint8_t)~MCD_PA;
                if (s_line_number == 0) s_line_number = 1;
            } else {
                s_csr1r |= MCD_PA;
            }
        } else {
            s_csr1r |= MCD_PA;
        }

        if (s_line_number <= 1)
            mcd212_video_begin_frame((uint16_t)mcd_base_width(),
                                     (uint16_t)mcd_active_height(),
                                     (s_reg[MCD_DCR1] & MCD_DCR1_SM) != 0);

        if (s_reg[MCD_DCR1] & (1u << 15)) {
            uint32_t vsr_a = mcd_get_vsr(0), vsr_b = mcd_get_vsr(1);
            unsigned base = mcd_base_width();
            uint16_t bytes_a = 0, bytes_b = 0;
            uint16_t width_a = (uint16_t)(base * ((s_reg[MCD_DCR1] & (1u << 11)) ? 2u : 1u));
            uint16_t width_b = (uint16_t)(base * ((s_reg[MCD_DCR2] & (1u << 11)) ? 2u : 1u));
            mcd212_video_draw_line(vsr_a, vsr_b, s_line_number,
                                   width_a, width_b, &bytes_a, &bytes_b);
            mcd_set_vsr(0, vsr_a + bytes_a);
            mcd_set_vsr(1, vsr_b + bytes_b);
            if ((s_reg[MCD_DCR1] & 0x0300u) == 0x0300u) mcd_execute_dca(0);
            if ((s_reg[MCD_DCR2] & 0x0300u) == 0x0300u) mcd_execute_dca(1);
        }
        s_line_number += (s_reg[MCD_DCR1] & MCD_DCR1_SM) ? 2u : 1u;

        if (s_vlines >= mcd_total_lines()) {        /* frame end -> retrace      */
            s_csr1r &= (uint8_t)~MCD_DA;            /* keeps PA (CeDImu &= 0x20) */
            s_vlines = 0;
            s_line_number = 0;
            mcd212_video_end_frame();
            g_frame_count++;
            cdi_input_schedule_advance(g_frame_count);
            debug_ring_capture_frame();
            if (g_stop_frame && g_frame_count >= g_stop_frame) cdi_fault_hold();
        }
    }
    /* Host pacing is deliberately outside the device-state update above, so a
     * concurrent debug query never observes a half-applied CPU cycle batch. */
    runtime_pace_cycles(cycles);
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
    mcd212_video_copy_frame(framebuf,
                            MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT,
                            NULL, NULL, NULL);
}

uint32_t mcd212_framebuffer_info(uint16_t *width, uint16_t *height,
                                 uint64_t *generation) {
    return mcd212_video_copy_frame(NULL, 0, width, height, generation);
}

uint64_t mcd212_framebuffer_hash(uint16_t *width, uint16_t *height,
                                 uint64_t *generation) {
    return mcd212_video_frame_hash(width, height, generation);
}

void mcd212_debug_state(uint16_t regs[32], uint8_t *csr1r,
                        uint8_t *csr2r, uint32_t *vline,
                        uint16_t *active_line) {
    if (regs) memcpy(regs, s_reg, sizeof s_reg);
    if (csr1r) *csr1r = s_csr1r;
    if (csr2r) *csr2r = s_csr2r;
    if (vline) *vline = s_vlines;
    if (active_line) *active_line = s_line_number;
}
