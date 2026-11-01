/* Native CRC verification spans in the real CD-RTOS module loader. The kernel
 * still owns file I/O, allocation, registration, errors and scheduling. Native
 * work folds an entire ready span, retaining the original device-time quanta.
 * CPU instruction detail is materialized only for an accepted interrupt or an
 * asynchronous debugger hold; the reference is never executed alongside it. */
#include "cdi_loader_jobs.h"
#include "debug_server.h"
#include "cdi_sha256.h"
#include <string.h>

#ifndef CDI_LOADER_HLE
#define CDI_LOADER_HLE 0
#endif

static CdiLoaderJobState stats;
RecompFuncPtr cdi_loader_entry;

#if CDI_LOADER_HLE
extern uint8_t g_rom[CDI_ROM_SIZE];
extern RecompFuncPtr recomp_native_find_entry(uint32_t address);
#include "cdi_loader_math.h"
static CrcFlight flight;

/* Exact firmware operations, not a guessed CRC polynomial/ABI. A regenerated
 * unsupported BIOS keeps the faithful path without ROM-specific link imports. */
static const uint16_t word_pc[] = {
    0x6BF4,0x6BF6,0x6BF8,0x6BFA,0x6BFC,0x6BFE,0x6C00,0x6C02,
    0x6C04,0x6C06,0x6C08,0x6C0E,0x6C10,0x6C12,0x6C14,0x6C16,
    0x6C1A,0x6C1C,0x6C1E,0x6C20,0x6C22,0x6C24,0x6C26,0x6C28,
    0x6C2A,0x6C2C,0x6C2E,0x6C30,0x6C34,0x6C36
};
static const uint8_t word_cycles[] = {
    37,15,7,7,7,7,7,7,7,7,15,7,28,7,7,14,22,7,7,82,7,7,7,37,7,7,22,21,13,7
};

static int irq_ready(void) {
    if (!g_recomp_irq_armed || !recomp_pending_irq_mask()) return 0;
    int level=recomp_pending_irq_level();
    return level==7 || level>((g_cpu.SR>>8)&7);
}
static void account(unsigned cycles) {
    g_native_insn_count++;
    g_cycle_accumulator+=cycles;g_audio_cycle_counter+=cycles;
    if (g_cycle_accumulator>=g_vblank_threshold) glue_check_vblank();
}
static int supported_span(uint32_t a, uint32_t n) {
    return (a<CDI_RAM0_SIZE && n<=CDI_RAM0_SIZE-a) ||
        (a>=CDI_RAM1_BASE && a-CDI_RAM1_BASE<CDI_RAM1_SIZE &&
         n<=CDI_RAM1_SIZE-(a-CDI_RAM1_BASE));
}

static void run_crc_span(void) {
    stats.spans++;
    for (;;) {
        g_cpu.PC=0x406BF4u;
        debug_trace_block(); /* job record and normal entry/hold/IRQ safepoint */
        flight.before=g_cpu;flight.completed=0;flight.pc=0x406BF4u;flight.word=0;flight.active=1;
        M68KState result;int odd=0;
        for (unsigned i=0;i<30;i++) {
            if (i==29 && !odd) break;
            flight.pc=CDI_ROM_BASE+word_pc[i];flight.completed=i;
            if (irq_ready()) {
                g_cpu=prefix(&flight);flight.active=0;stats.interrupted++;
                recomp_take_irq();
            }
            if (i==1) {
                flight.word=m68k_read32(flight.before.A[0]);
                result=fold_word(&flight.before,flight.word,&odd);
            }
            if (i==27)
                (void)m68k_read8(CDI_ROM_BASE+0x6BC0u+((result.D[0]&255u)>>3));
            /* Generated register shifts account time before changing the
             * destination. Keep that phase at C20, including debugger holds. */
            flight.completed=i==19?i:i+1;
            g_cpu.PC=flight.pc;
            account(word_cycles[i]);
        }
        flight.active=0;g_cpu=result;stats.words++;
        /* Kernel scheduling check at each 256-word boundary remains the
         * faithful caller's operation, including any process-state mutation. */
        if (!(g_cpu.D[3]&255u)) {stats.yielded++;recomp_tail_call(0x406C38u);return;}
        /* A completed checksum word owns these three loop-control operations;
         * materialize them directly at each possible interrupt boundary. */
        g_cpu.PC=0x406C38u;
        if(irq_ready())recomp_take_irq();
        logical(&g_cpu,g_cpu.D[3],8);account(7);
        g_cpu.PC=0x406C3Au;
        if(irq_ready())recomp_take_irq();
        account(13);
        g_cpu.PC=0x406C5Eu;
        if(irq_ready())recomp_take_irq();
        g_cpu.D[3]=(g_cpu.D[3]&0xFFFF0000u)|((g_cpu.D[3]-1u)&0xFFFFu);
        account(17);
    }
}

int cdi_loader_dispatch(RecompFuncPtr function) {
    if (!cdi_loader_entry || function!=cdi_loader_entry || (g_cpu.A[0]&1u) ||
        g_cpu.D[3]>0xFFFFu || !supported_span(g_cpu.A[0],(g_cpu.D[3]+1u)*4u) ||
        !debug_native_job_allowed(0x406BF4u,0x406C38u)) return 0;
    run_crc_span();return 1;
}
void cdi_loader_sync_cpu(void) {if(flight.active)g_cpu=prefix(&flight);}
void cdi_loader_reset(void) {
    static const uint8_t expected[32]={
        0x60,0xD3,0x4E,0x54,0xFC,0x15,0x54,0x84,0xA2,0x39,0x12,0x91,0x14,0x9D,0x76,0x2E,
        0xB9,0x1D,0x1D,0xA0,0xF0,0x5C,0x65,0xEE,0x4E,0xB3,0x54,0x98,0x66,0xA6,0xB9,0x1C
    };
    uint8_t digest[32];
    memset(&stats,0,sizeof stats);memset(&flight,0,sizeof flight);
    cdi_loader_entry=recomp_native_find_entry(0x406BF4u);
    cdi_sha256(g_rom+0x6BC0,0xA8,digest);
    if(memcmp(digest,expected,sizeof expected))cdi_loader_entry=NULL;
}
#else
int cdi_loader_dispatch(RecompFuncPtr function) {(void)function;return 0;}
void cdi_loader_sync_cpu(void) {}
void cdi_loader_reset(void) {memset(&stats,0,sizeof stats);cdi_loader_entry=NULL;}
#endif
void cdi_loader_job_state(CdiLoaderJobState *out) {*out=stats;}
