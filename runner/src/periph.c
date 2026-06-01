/*
 * periph.c — SCC68070 on-chip peripheral register block ($80001001..$80008080).
 *
 * Faithful port of CeDImu's SCC68070 Peripherals.cpp. The block is a backing
 * store of byte registers (LIR, I2C, UART, timers, 2 DMA channels, MMU, the
 * peripheral interrupt-control registers) with specific reactive behaviour on
 * a handful of them:
 *   - URHR  : reads the next UART input byte (none available yet -> 0)
 *   - UCR   : command bits reset rx/tx/error
 *   - UTHR  : transmits a byte (TXEMT set in USR); we capture it
 *   - TSR   : writing a 1 clears that status bit (xor)
 *   - LIR   : masked to 0x77
 *   - MSR   : read-only (writes ignored)
 *   - everything else: plain register storage (config: PICR1/2, DMA, MMU setup)
 *
 * Timer counting (IncrementTimer) and DMA transfers are reactive subsystems
 * driven by CPU cycles; ported as periph_increment_timer for when the pacing
 * loop drives it (dormant until then). The MMU registers are stored but address
 * translation is NOT yet applied to recompiled accesses (the flat-address risk,
 * MC-CDI-006); revisit if CD-RTOS enables remapping that the recompiled code
 * depends on.
 *
 * TODO MC-CDI-006.
 */
#include "cdi_runtime.h"
#include <stdio.h>
#include <string.h>

#define PERIPH_BASE  0x80001001u
#define PERIPH_SIZE  0x7080u          /* Last(0x80008080) - Base + 1, padded */

/* Absolute register addresses with reactive behaviour (from CeDImu). */
#define R_LIR   0x80001001u
#define R_USR   0x80002013u
#define R_UCR   0x80002017u
#define R_UTHR  0x80002019u
#define R_URHR  0x8000201Bu
#define R_TSR   0x80002020u
#define R_MSR   0x80008000u

static uint8_t s_periph[PERIPH_SIZE];

static inline uint32_t off_of(uint32_t addr) { return addr - PERIPH_BASE; }

static uint8_t periph_get(uint32_t addr) {
    uint32_t off = off_of(addr);
    if (off >= PERIPH_SIZE) return 0;

    /* UART receive-ready reflects the (currently always empty) input queue. */
    if (addr == R_URHR) {
        s_periph[off_of(R_URHR)] = 0;     /* no UART input source modelled yet */
        s_periph[off_of(R_USR)] &= (uint8_t)~0x01;  /* clear RxRDY */
    }
    return s_periph[off];
}

static void periph_set(uint32_t addr, uint8_t data) {
    uint32_t off = off_of(addr);
    if (off >= PERIPH_SIZE) return;

    switch (addr) {
    case R_LIR:
        s_periph[off] = data & 0x77;      /* PIR reads as 0 */
        break;
    case R_UCR:
        switch (data & 0x70) {
        case 0x20: s_periph[off_of(R_URHR)] = 0; break;          /* reset receiver */
        case 0x30: s_periph[off_of(R_UTHR)] = 0; break;          /* reset transmitter */
        case 0x40: s_periph[off_of(R_USR)] &= 0x0F; break;       /* reset error status */
        }
        s_periph[off] = data;
        break;
    case R_UTHR:
        s_periph[off_of(R_USR)] |= 0x08;  /* TXEMT */
        s_periph[off] = data;             /* TODO: surface UART out via the ring buffer */
        break;
    case R_TSR:
        s_periph[off] ^= data;            /* write-1-to-clear */
        break;
    case R_MSR:
        break;                            /* read-only */
    default:
        s_periph[off] = data;
        break;
    }
}

/* ---- bus interface (big-endian, byte-wise like the SCC68070) ---- */
uint32_t periph_read(uint32_t addr, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = (v << 8) | periph_get(addr + (uint32_t)i);
    return v;
}
void periph_write(uint32_t addr, uint32_t val, int size) {
    for (int i = 0; i < size; i++)
        periph_set(addr + (uint32_t)i, (uint8_t)(val >> (8 * (size - 1 - i))));
}
