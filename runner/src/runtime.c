/*
 * runtime.c — CPU-state ABI, exception handling, frame pacing, dispatch-miss
 * accounting. The non-memory half of the contract in cdi_runtime.h.
 *
 * Skeleton state: control-flow plumbing is in place; the pieces that need a
 * running CD-RTOS (boot, pacing, interrupts) FAIL LOUD rather than pretending.
 */
#include "cdi_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- CPU + ABI globals (generated code references these) ---- */
M68KState g_cpu;
uint64_t  g_frame_count       = 0;
uint64_t  g_native_insn_count = 0;
uint32_t  g_cycle_accumulator = 0;
uint32_t  g_vblank_threshold  = 0;
uint32_t  g_audio_cycle_counter = 0;

static int s_rte_pending = 0;
int *g_rte_pending_ptr = &s_rte_pending;
int  g_early_return    = 0;

/* ---- Dispatch-miss monitor ---- */
uint32_t g_miss_count_any = 0;
uint32_t g_miss_last_addr = 0;
uint64_t g_miss_last_frame = 0;
uint32_t g_miss_unique_addrs[CDI_MAX_MISS_UNIQUE];
int      g_miss_unique_count = 0;

void genesis_log_dispatch_miss(uint32_t addr) {
    g_miss_count_any++;
    g_miss_last_addr  = addr;
    g_miss_last_frame = g_frame_count;
    for (int i = 0; i < g_miss_unique_count; i++)
        if (g_miss_unique_addrs[i] == addr) return;
    if (g_miss_unique_count < CDI_MAX_MISS_UNIQUE)
        g_miss_unique_addrs[g_miss_unique_count++] = addr;
    fprintf(stderr, "[dispatch-miss] no generated function at $%08X (frame %llu)\n",
            addr, (unsigned long long)g_frame_count);
}

/* Default dispatch override: no hand-written handler. A specific OS/game module
 * may override this. Generated dispatch falls back to the miss logger. */
int game_dispatch_override(uint32_t addr) { (void)addr; return 0; }

/* ---- Runtime init ---- */
void runtime_init(void) {
    memset(&g_cpu, 0, sizeof g_cpu);
    /* TODO MC-CDI-001: load CD-RTOS system ROM, run the OS-9 module loader to
     * relocate the boot module (cdi_hotel) into RAM, then seed g_cpu.A[7]/PC
     * from its execution entry. Until then there is no valid start state. */
}

/* ---- Frame pacing / interrupts (need a running CD-RTOS) ---- */
void glue_check_vblank(void)            { /* TODO MC-CDI-007: cycle-accurate VSYNC from MCD212 timing */ }
void glue_yield_for_vblank(void)        { /* TODO MC-CDI-007: fiber yield for frame pacing */ }
void glue_yield_for_interrupt_poll(void){ /* TODO MC-CDI-007 */ }
void runtime_request_vblank(void)       { /* TODO MC-CDI-007 */ }

/* ---- Privileged / exception semantics ---- */
void genesis_reset_devices(void) {
    /* RESET instruction: re-initialise MCD212 / CDIC / SLAVE. TODO MC-CDI-008. */
}
void genesis_stop_until_interrupt(uint16_t sr_imm) {
    g_cpu.SR = sr_imm;
    /* TODO MC-CDI-007: halt until an IRQ above the I-mask; yield to pacing. */
}
void m68k_trap_vector(uint8_t vec) {
    if (vec == 0x20) {            /* TRAP #0 = OS-9 / CD-RTOS system-call gateway */
        cdrtos_syscall();
        return;
    }
    fprintf(stderr, "[trap] unhandled vector 0x%02X at PC=$%08X (see TODO MC-CDI-010)\n",
            vec, g_cpu.PC);
    abort();
}
void m68k_illegal_trap(uint32_t pc, uint16_t opcode) {
    fprintf(stderr, "[trap] ILLEGAL/A-line/F-line opcode 0x%04X at PC=$%08X\n", opcode, pc);
    abort();
}

/* MOVEC control-register access. The SCC68070 control-register model is not
 * built yet (TODO MC-CDI-006). Fail loud with the exact control code rather
 * than fabricate a value — the ROM's MOVEC sites are CPU-type-gated cache
 * control (CACR, code 0x002) that never executes on the SCC68070, so reaching
 * here is a genuine surprise worth stopping on. */
uint32_t m68k_movec_read(uint16_t cc) {
    fprintf(stderr, "[movec] read of unmodelled control register 0x%03X at PC=$%08X (TODO MC-CDI-006)\n",
            cc, g_cpu.PC);
    abort();
}
void m68k_movec_write(uint16_t cc, uint32_t val) {
    fprintf(stderr, "[movec] write 0x%08X to unmodelled control register 0x%03X at PC=$%08X (TODO MC-CDI-006)\n",
            val, cc, g_cpu.PC);
    abort();
}

/* ---- Interpreter fallbacks (not modelled yet) ---- */
void hybrid_jmp_interpret(uint32_t target_pc) {
    fprintf(stderr, "[hybrid] JMP interpret @$%08X not implemented (TODO MC-CDI-011)\n", target_pc);
    abort();
}
void hybrid_call_interpret(uint32_t target_pc) {
    fprintf(stderr, "[hybrid] call interpret @$%08X not implemented (TODO MC-CDI-011)\n", target_pc);
    abort();
}

/* ---- misc ---- */
void log_on_change(const char *label, uint32_t value) {
    static uint32_t last = 0; static const char *last_label = NULL;
    if (label != last_label || value != last) {
        fprintf(stderr, "[log] %s = 0x%08X\n", label, value);
        last = value; last_label = label;
    }
}
