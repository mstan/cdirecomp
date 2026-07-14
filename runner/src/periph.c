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
 * Timer counting (IncrementTimer) is driven by CPU cycles from the same edge
 * that advances the other devices. DMA transfers remain pending. The MMU
 * registers are stored but address translation is NOT yet applied to
 * recompiled accesses (the flat-address risk, MC-CDI-006); revisit if CD-RTOS
 * enables remapping that the recompiled code depends on.
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
#define R_TCR   0x80002021u
#define R_RRH   0x80002022u
#define R_RRL   0x80002023u
#define R_T0H   0x80002024u
#define R_T0L   0x80002025u
#define R_T1H   0x80002026u
#define R_T1L   0x80002027u
#define R_T2H   0x80002028u
#define R_T2L   0x80002029u
#define R_PICR1 0x80002045u
#define R_MSR   0x80008000u

static uint8_t s_periph[PERIPH_SIZE];

/* Keep the timer clock in the same domain as CeDImu.  Its SCC68070 stores
 * m_cycleDelay and m_timerCounter as double nanoseconds, adding one complete
 * CPU execution quantum at a time.  An exact integer modulo-96 counter is
 * mathematically cleaner but not behaviorally identical: floating-point
 * rounding can move an overflow across one STOP quantum after a long run,
 * which changes the instruction at which OS-9 receives the timer IRQ. */
static const double s_timer_cycle_delay_ns =
    (double)((1.0L / 15104900.0L) * 1000000000.0L);
static double s_timer_counter_ns;

static inline uint32_t off_of(uint32_t addr) { return addr - PERIPH_BASE; }

/* Reset the on-chip peripherals (mirror CeDImu SCC68070::Reset). The boot polls
 * USR bit 2 (TxRDY, transmitter ready) before sending UART bytes; CeDImu sets it
 * at reset and never clears it (our transmitter has no serial delay, so it is
 * always ready), so initialise it the same way or the boot spins forever. */
void periph_reset(void) {
    memset(s_periph, 0, sizeof s_periph);
    s_timer_counter_ns = 0.0;
    s_periph[off_of(R_USR)] = 0x04;   /* SET_TX_READY */
}

uint8_t periph_lir(void) {
    return s_periph[off_of(R_LIR)];
}

static uint16_t timer_get16(uint32_t high_addr) {
    return (uint16_t)((uint16_t)s_periph[off_of(high_addr)] << 8 |
                      s_periph[off_of(high_addr + 1)]);
}

static void timer_set16(uint32_t high_addr, uint16_t value) {
    s_periph[off_of(high_addr)] = (uint8_t)(value >> 8);
    s_periph[off_of(high_addr + 1)] = (uint8_t)value;
}

/* SCC68070 timer prescaler: CeDImu derives m_timerDelay from its rounded
 * double m_cycleDelay, then accumulates the double nanoseconds supplied by
 * each CPU execution quantum. T0 always runs; T1/T2 are gated by their TCR
 * mode fields. Each overflow queues the PICR1-selected on-chip autovector.
 * This is intentionally called once for every CPU execution quantum,
 * including STOP's 25-cycle quanta. */
void periph_increment_timer(uint32_t cycles) {
    const double timer_delay_ns = s_timer_cycle_delay_ns * 96.0;
    s_timer_counter_ns += (double)cycles * s_timer_cycle_delay_ns;
    while (s_timer_counter_ns >= timer_delay_ns) {
        s_timer_counter_ns -= timer_delay_ns;
        uint8_t *tsr = &s_periph[off_of(R_TSR)];
        uint8_t tcr = s_periph[off_of(R_TCR)];
        uint8_t priority = s_periph[off_of(R_PICR1)] & 7u;
        uint16_t t0 = timer_get16(R_T0H);

        if (t0 == 0xFFFFu) {
            *tsr |= 0x80u;
            t0 = timer_get16(R_RRH);
            if (priority) cdi_irq_raise_onchip_level(priority);
        } else {
            t0++;
        }
        timer_set16(R_T0H, t0);

        if (tcr & 0x30u) {
            uint16_t t1 = timer_get16(R_T1H);
            if (t1 == 0xFFFFu) {
                *tsr |= 0x10u;
                if (priority) cdi_irq_raise_onchip_level(priority);
            } else if ((tcr & 0x30u) == 0x30u) {
                *tsr &= 0xEEu;
            }
            t1++;
            if (t1 == t0) *tsr |= 0x40u;
            timer_set16(R_T1H, t1);
        }

        if (tcr & 0x03u) {
            uint16_t t2 = timer_get16(R_T2H);
            if (t2 == 0xFFFFu) {
                *tsr |= 0x02u;
                if (priority) cdi_irq_raise_onchip_level(priority);
            } else if ((tcr & 0x03u) == 0x03u) {
                *tsr &= 0xFCu;
            }
            t2++;
            if (t2 == t0) *tsr |= 0x08u;
            timer_set16(R_T2H, t2);
        }
    }
}

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
