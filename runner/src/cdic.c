/*
 * cdic.c — CIAP: CD Interface and Audio Processor (Mono-3/4 board).
 *
 * On the Mono-3 board — which CeDImu maps cdi490a (Mono-IV) to — the CD
 * interface at $300000..$304000 is the CIAP, NOT the Mono-1/2 "CDIC" chip.
 * (The RGN_CDIC / cdic_* / CDI_CDIC_BASE names in the bus layer are legacy;
 * the region and entry points are kept, the model is the CIAP.)
 *
 * CeDImu models the CIAP as HLE: a flat 16-bit register array with exactly two
 * pieces of live behavior —
 *   - ID ($25C4) reads a fixed signature 0xCD02. CD-RTOS probes it to detect
 *     the CIAP during device init; THIS is the read that previously fail-stopped
 *     the boot at $3025C4.
 *   - ISR_221 ($2586) is the data-interrupt status. CeDImu's CIAP::IncrementTime
 *     sets it to 9 and GetWord clears it on read — but IncrementTime runs after
 *     EVERY instruction (Interpreter.cpp), so a guest read ALWAYS observes 9.
 *     Our flat-call runtime has no per-instruction hook, so the faithful
 *     observable equivalent is simply: ISR_221 reads return 9.
 * Every other offset is plain storage: writes < 0x2600 stick, reads return the
 * stored word (0 at reset, except ID). The CIAP raises NO CPU interrupt in
 * CeDImu — the display IRQ that wakes the shell-idle STOP comes from the MCD212
 * via INT1 (MC-CDI-007), not from here.
 *
 * Faithful mirror of:
 *   external/CeDImu/src/CDI/HLE/CIAP/CIAP.{cpp,hpp}   (register map + behavior)
 *   external/CeDImu/src/CDI/boards/Mono3/Bus.cpp      (byte/word access split)
 * TODO MC-CDI-013.
 */
#include "cdi_runtime.h"
#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CIAP register offsets (CeDImu HLE::CIAP::Registers). Only ID and ISR_221 have
 * live behavior; the rest are documented register slots backed by plain storage. */
enum {
    CIAP_ISR_221   = 0x2586,   /* data-interrupt status (reads 9)              */
    CIAP_ID        = 0x25C4,   /* fixed signature 0xCD02                       */
    CIAP_REGS_SIZE = 0x2600,   /* documented register window (CeDImu _size)    */
};

static uint16_t s_reg[CIAP_REGS_SIZE / 2];
static int      s_inited = 0;

/* Every CPU-side CIAP access, retained independently of the generic store
 * ring so reads (including read-to-clear registers) and dropped firmware-window
 * writes remain visible.  64K entries is intentionally large enough to query
 * backward through boot-time polling without an armed trace. */
#define CIAP_EVENT_CAP (1u << 16)
#define CIAP_EVENT_MASK (CIAP_EVENT_CAP - 1u)
static CdiCiapEvent s_events[CIAP_EVENT_CAP];
static uint64_t s_event_total;

static void ciap_record_access(uint32_t off, uint32_t val, int size, int write) {
    CdiCiapEvent *e = &s_events[s_event_total & CIAP_EVENT_MASK];
    e->seq = s_event_total++;
    e->trace_seq = debug_trace_sequence();
    e->frame = g_frame_count;
    e->cycles = g_total_cycles;
    e->pc = g_cpu.PC;
    e->offset = off;
    e->value = val;
    e->size = (uint8_t)size;
    e->write = (uint8_t)(write != 0);
}

int cdic_debug_events(CdiCiapEvent *out, int capacity, uint64_t from,
                      uint64_t *total, uint64_t *oldest) {
    uint64_t end = s_event_total;
    uint64_t begin = end > CIAP_EVENT_CAP ? end - CIAP_EVENT_CAP : 0;
    if (from == UINT64_MAX) {
        uint64_t wanted = capacity > 0 ? (uint64_t)capacity : 0;
        from = end > wanted ? end - wanted : 0;
    }
    if (from < begin) from = begin;
    if (from > end) from = end;
    int count = 0;
    for (uint64_t seq = from; seq < end && count < capacity; seq++)
        out[count++] = s_events[seq & CIAP_EVENT_MASK];
    if (total) *total = end;
    if (oldest) *oldest = begin;
    return count;
}

static void ciap_init(void) {
    memset(s_reg, 0, sizeof s_reg);
    s_reg[CIAP_ID >> 1] = 0xCD02;
    s_inited = 1;
}

/* Word value at a word-aligned CIAP offset, mirroring CeDImu CIAP::GetWord. */
static uint16_t ciap_get_word(uint32_t woff) {
    if (woff == CIAP_ISR_221)
        return 9;                    /* IncrementTime re-sets it every instruction */
    return s_reg[woff >> 1];
}

uint32_t cdic_read(uint32_t addr, int size) {
    if (!s_inited) ciap_init();
    uint32_t off = addr - CDI_CDIC_BASE;
    if (off >= CIAP_REGS_SIZE) {
        /* CeDImu CIAP::GetWord indexes its register array unguarded, so a read
         * here is out-of-bounds (UB) — there is no faithful value to return.
         * The boot only WRITES beyond the window (CIAP firmware download), so a
         * read here is a genuine "we don't know what the oracle returns": fail
         * loud rather than invent a value. */
        fprintf(stderr, "[ciap] read%d @ $%08X (off $%X) beyond register window "
                        "— CeDImu reads OOB here (TODO MC-CDI-013)\n",
                        size * 8, addr, off);
        debug_dump_fault_trail("CIAP read beyond register window");
        if (g_hold_on_fault) cdi_fault_hold();
        abort();
    }

    uint16_t w = ciap_get_word(off & ~1u);
    uint32_t result = size == 1
        ? ((off & 1) ? (w & 0xFF) : (w >> 8))       /* CeDImu Mono3::GetByte */
        : w;
    ciap_record_access(off, result, size, 0);
    return result;
}

void cdic_write(uint32_t addr, uint32_t val, int size) {
    if (!s_inited) ciap_init();
    uint32_t off = addr - CDI_CDIC_BASE;
    ciap_record_access(off, val, size, 1);

    /* CeDImu CIAP::SetWord stores ONLY offsets < 0x2600 ("if(addr < 0x2600)");
     * writes beyond the documented window are silently dropped. The CD-RTOS CD
     * driver downloads CIAP firmware into offsets that run past 0x2600 (a
     * MOVE.W (A0)+,(A4)+ block copy from $42795C); those writes are no-ops in
     * CeDImu, so dropping them here is the faithful behavior — NOT a fault. */
    if (off >= CIAP_REGS_SIZE)
        return;

    /* Byte writes read-modify-write the addressed byte half (Mono3::SetByte). */
    uint32_t idx = off >> 1;
    if (size == 1) {
        uint16_t w = s_reg[idx];
        if (off & 1) w = (uint16_t)((w & 0xFF00) | (uint8_t)val);
        else         w = (uint16_t)((w & 0x00FF) | ((uint16_t)(uint8_t)val << 8));
        s_reg[idx] = w;
    } else {
        s_reg[idx] = (uint16_t)val;
    }
}
