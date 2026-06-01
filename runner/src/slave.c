/*
 * slave.c — IKAT input/serial-gate MCU (HLE), ported from CeDImu HLE::IKAT.
 *
 * The CD-i input MCU sits at $310000 on the odd bytes; register index =
 * (addr - $310000) >> 1. On Mono-3/4 (cdi490a = Mono-4, implemented by the
 * Mono3 board in CeDImu) and Mono-2 this gate is the IKAT (the "SLAVE" naming
 * is Mono-1/2 legacy). The CD-RTOS boot talks to it over four byte channels
 * (A-D) with a command/response protocol: it queries the IKAT firmware
 * version, the boot mode (player vs service shell), the video standard
 * (NTSC/PAL), and disc status.
 *
 * 15 host-visible registers:
 *   0-3   CHx_IN   host writes command bytes
 *   4-7   CHx_OUT  host reads response bytes
 *   8-11  CHx_SR   channel status (bit0/bit4 = ready/empty flags)
 *   12    ISR      interrupt status
 *   13    IMR      interrupt mask
 *   14    Mode/YCR (writes are no-ops)
 *
 * Faithful mirror of external/CeDImu/src/CDI/HLE/IKAT/IKAT.{cpp,hpp}. Channel-D
 * disc responses are 2-frame delayed (driven by slave_increment_frame, which
 * the pacing loop will call once frame timing exists; dormant until then —
 * disc commands come after the CD subsystem is up).
 *
 * TODO MC-CDI-014 / MC-CDI-023.
 */
#include "cdi_runtime.h"
#include <stdio.h>
#include <string.h>

/* ---- register map ---- */
enum { CHA_IN=0, CHB_IN, CHC_IN, CHD_IN,
       CHA_OUT, CHB_OUT, CHC_OUT, CHD_OUT,
       CHA_SR,  CHB_SR,  CHC_SR,  CHD_SR,
       ISR, IMR, MODE, IKAT_NREG };
enum { CHA=0, CHB, CHC, CHD };

#define UNSET_REMTY(reg) (s_reg[reg] &= 0x01)
#define SET_RTEMTY(reg)  (s_reg[reg] |= 0x11)
#define CHANNEL(addr)    ((addr) & 3)

static const int INT_MASK[4] = { 0x02, 0x08, 0x20, 0x80 };

/* Video standard: Hotel Mario USA is NTSC (PAL=0). responseCF6[2] = PAL+1. */
static int s_pal = 0;

static uint8_t s_reg[IKAT_NREG];

/* Per-channel input (host->MCU command accumulation) and output (MCU->host
 * response) byte queues. Commands/responses are <= 4 bytes. */
static uint8_t s_in[4][8];   static int s_in_len[4];
static uint8_t s_out[4][8];  static int s_out_pos[4]; static int s_out_len[4];

/* 2-frame-delayed channel responses (disc queries). */
static const uint8_t *s_delayed_rsp[4]; static int s_delayed_len[4];
static uint64_t       s_delayed_frame[4];

/* Command responses (https://github.com/cdifan/cdichips/blob/master/mc6805ikat.md). */
static const uint8_t responseCF0[3] = { 0xA5, 0xF0, 0x7F }; /* IKAT release number   */
static       uint8_t responseCF6[4] = { 0xA5, 0xF6, 1, 0xFF }; /* video std: [2]=PAL+1 */
static const uint8_t responseCF4[3] = { 0xA5, 0xF4, 0 };   /* boot mode: 0=player shell */
static const uint8_t responseCF7[3] = { 0xA5, 0xF7, 0 };   /* CD60 low-level test (0=ok) */
static const uint8_t responseCF8[3] = { 0xA5, 0xF8, 0 };   /* DSIC2 low-level test (0=ok) */
static const uint8_t responseDB0[4] = { 0xB0, 0x02, 0x10, 0 }; /* disc status   */
static const uint8_t responseDB1[4] = { 0xB1, 0, 2, 0 };       /* disc base     */
static const uint8_t responseDB2[4] = { 0xB2, 0x20, 0, 0x10 }; /* disc select   */

static int  s_inited = 0;

static void ikat_init(void) {
    memset(s_reg, 0, sizeof s_reg);
    memset(s_in_len, 0, sizeof s_in_len);
    memset(s_out_pos, 0, sizeof s_out_pos);
    memset(s_out_len, 0, sizeof s_out_len);
    memset(s_delayed_rsp, 0, sizeof s_delayed_rsp);
    responseCF6[2] = (uint8_t)(s_pal + 1);
    for (int reg = CHA_SR; reg <= CHD_SR; reg++) SET_RTEMTY(reg);
    s_inited = 1;
}

static void set_int(int ch) {
    s_reg[ISR] |= (uint8_t)INT_MASK[ch];
    if (s_reg[IMR] & INT_MASK[ch])
        cdi_irq_raise(2);            /* IKAT asserts level-2; delivery is a later milestone */
}

static void out_set(int ch, const uint8_t *rsp, int n) {
    memcpy(s_out[ch], rsp, (size_t)n);
    s_out_pos[ch] = 0;
    s_out_len[ch] = n;
}
static int out_size(int ch) { return s_out_len[ch] - s_out_pos[ch]; }

static void delay_rsp(int ch, const uint8_t *rsp, int n) {
    s_delayed_rsp[ch]   = rsp;
    s_delayed_len[ch]   = n;
    s_delayed_frame[ch] = g_frame_count + 2;
}

static void process_command_c(void) {
    switch (s_in[CHC][0]) {
    case 0xF0: s_in_len[CHC]=0; out_set(CHC, responseCF0, 3); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    case 0xF4: s_in_len[CHC]=0; out_set(CHC, responseCF4, 3); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    case 0xF6: s_in_len[CHC]=0; out_set(CHC, responseCF6, 4); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    case 0xF7: s_in_len[CHC]=0; out_set(CHC, responseCF7, 3); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    case 0xF8: s_in_len[CHC]=0; out_set(CHC, responseCF8, 3); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    default:   s_in_len[CHC]=0; break;
    }
}

static void process_command_d(void) {
    switch (s_in[CHD][0]) {
    case 0xA1: if (s_in_len[CHD]==4) { s_in_len[CHD]=0; delay_rsp(CHD, responseDB0, 4); } break;
    case 0xB0: if (s_in_len[CHD]==4) { s_in_len[CHD]=0; delay_rsp(CHD, responseDB0, 4); } break;
    case 0xB1: s_in_len[CHD]=0; out_set(CHD, responseDB1, 4); UNSET_REMTY(CHD_SR); set_int(CHD); break;
    case 0xB2: if (s_in_len[CHD]==4) { s_in_len[CHD]=0; delay_rsp(CHD, responseDB2, 4); } break;
    default:   s_in_len[CHD]=0; break;
    }
}

/* Called once per frame by the pacing loop (when it exists) to fire delayed
 * disc responses. Dormant until frame timing is modelled. */
void slave_increment_frame(void) {
    for (int ch = CHA; ch <= CHD; ch++) {
        if (s_delayed_rsp[ch] != NULL && s_delayed_frame[ch] == g_frame_count) {
            out_set(ch, s_delayed_rsp[ch], s_delayed_len[ch]);
            UNSET_REMTY(CHA_SR + ch);
            set_int(ch);
            s_delayed_rsp[ch] = NULL;
            s_delayed_frame[ch] = 0;
        }
    }
}

static uint8_t ikat_get(uint8_t reg) {
    int ch = CHANNEL(reg);
    if (reg >= CHA_OUT && reg <= CHD_OUT && out_size(ch) > 0) {
        s_reg[reg] = s_out[ch][s_out_pos[ch]++];
        if (out_size(ch) == 0) {
            s_reg[ISR] &= (uint8_t)~(1 << (1 + 2 * ch));
            SET_RTEMTY(CHA_SR + ch);
        } else {
            UNSET_REMTY(CHA_SR + ch);
        }
    }
    return s_reg[reg];
}

static void ikat_set(uint8_t reg, uint8_t data) {
    if (reg == IMR) {
        s_reg[IMR] = data;
    } else if (reg <= CHD_IN) {           /* CHA_IN..CHD_IN */
        s_reg[reg] = data;
        int ch = CHANNEL(reg);
        if (s_in_len[ch] < (int)sizeof s_in[ch]) s_in[ch][s_in_len[ch]++] = data;
        if (reg == CHC_IN) process_command_c();
        else if (reg == CHD_IN) process_command_d();
    }
    /* reg 14 (Mode/YCR) and the OUT/SR/ISR registers: writes are ignored. */
}

/* ---- bus interface ---- */
static int addr_to_reg(uint32_t addr) {
    /* IKAT lives on the odd bytes; register index = (addr - base) >> 1. */
    return (int)((addr - CDI_SLAVE_BASE) >> 1);
}

uint32_t slave_read(uint32_t addr, int size) {
    if (!s_inited) ikat_init();
    int reg = addr_to_reg(addr);
    if (reg < 0 || reg >= IKAT_NREG) {
        fprintf(stderr, "[ikat] read%d @ $%08X -> reg %d out of range\n", size*8, addr, reg);
        return 0;
    }
    return ikat_get((uint8_t)reg);
}

void slave_write(uint32_t addr, uint32_t val, int size) {
    if (!s_inited) ikat_init();
    int reg = addr_to_reg(addr);
    if (reg < 0 || reg >= IKAT_NREG) {
        fprintf(stderr, "[ikat] write%d @ $%08X = $%X -> reg %d out of range\n",
                size*8, addr, val, reg);
        return;
    }
    ikat_set((uint8_t)reg, (uint8_t)val);
}
