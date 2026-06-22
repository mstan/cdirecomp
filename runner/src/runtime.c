/*
 * runtime.c — CPU-state ABI, exception handling, frame pacing, dispatch-miss
 * accounting. The non-memory half of the contract in cdi_runtime.h.
 *
 * Skeleton state: control-flow plumbing is in place; the pieces that need a
 * running CD-RTOS (boot, pacing, interrupts) FAIL LOUD rather than pretending.
 */
#include "cdi_runtime.h"
#include "debug_server.h"
#include "m68k_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Generated dispatch-table accessors (bios/generated/cdrtos_dispatch.c). */
int      game_dispatch_table_size(void);
uint32_t game_dispatch_table_addr(int i);

/* Is `addr` the entry of a statically recompiled function? (Used by the hybrid
 * interpreter to know when it has re-entered recompiled territory.) */
int dispatch_has_addr(uint32_t addr) {
    addr &= 0xFFFFFFu;
    int n = game_dispatch_table_size();
    for (int i = 0; i < n; i++)
        if (game_dispatch_table_addr(i) == addr) return 1;
    return 0;
}

/* ---- Hybrid interpreter handoff (MC-CDI-011) ----
 * CD-RTOS builds vector stubs and relocates modules into RAM, so indirect
 * control flow reaches code the static recompiler never saw (a "dispatch
 * miss"). Rather than no-op it, interpret the un-recompiled gap on the clean-
 * room m68k_interp until execution re-enters a recompiled function, then resume
 * native there. `ret` is the return address on the guest stack (optional stop).
 * Returns 1 if it handled the target, 0 to fall through to miss logging. */
static int s_in_hybrid = 0;

/* One-shot fallback-reason override for the NEXT interpreter entry. recomp_bus_error
 * dispatches the OS-9 bus handler a moment later via the trampoline (→
 * recomp_top_resume), with no intervening interpreter run, so a one-shot set here
 * is consumed exactly there. Default attribution otherwise comes from the entry. */
static int s_pending_fallback_reason = FB_NONE;

static int hybrid_enter(uint32_t target, uint32_t ret) {
    if (s_in_hybrid) return 0;           /* nested miss: log it rather than recurse blindly */
    s_in_hybrid = 1;
    int saved_reason = g_fallback_reason;
    g_fallback_reason = FB_DISPATCH_MISS;   /* attribute this run: no compiled function */
    M68kiStatus st = m68k_interp_run_until_known(target, ret);
    g_fallback_reason = saved_reason;
    s_in_hybrid = 0;

    if (st == (M68kiStatus)M68KI_REENTER) {
        recomp_call_addr(g_cpu.PC);      /* resume native at the recompiled entry */
        return 1;
    }
    if (st == M68KI_OK) return 1;        /* interpreted cleanly back to `ret` */

    fprintf(stderr, "[hybrid] interp halt(%d) entering $%06X: opcode $%04X at $%06X\n",
            st, target, g_m68ki_bad_op, g_m68ki_bad_pc);
    debug_dump_fault_trail("hybrid interp halt");
    return 0;                            /* let the dispatch-miss path record it */
}

/* Depth-0 resume for the top-level trampoline. See cdi_runtime.h. A recompiled
 * entry is dispatched flat-call (its RTS bottoms back to the loop, which then
 * follows [A7] for the next return). A non-entry target is interpreted with NO
 * ret-stop: its RTS self-pops the next return and execution FLOWS into it, so we
 * run until re-entering a recompiled entry, then dispatch that. This is what the
 * loop's plain [A7]-follow gets wrong for the `pea ret; pea tgt; rts` idiom — the
 * hybrid callee pops `ret` itself, and a second [A7]-follow would double-pop. */
void recomp_top_resume(uint32_t addr) {
    addr &= 0xFFFFFFu;
    if (dispatch_has_addr(addr)) {
        recomp_call_addr(addr);              /* recompiled entry: flat-call */
        return;
    }
    /* Attribute this interpreter run: a bus/exception handler dispatched via the
     * trampoline carries a one-shot reason; otherwise it's a plain depth-0 target. */
    g_fallback_reason = s_pending_fallback_reason ? s_pending_fallback_reason : FB_TOP_RESUME;
    s_pending_fallback_reason = FB_NONE;
    M68kiStatus st = m68k_interp_run_until_known(addr, 0);  /* stop_pc=0: no ret-stop */
    if (st == (M68kiStatus)M68KI_REENTER) {
        recomp_call_addr(g_cpu.PC);          /* re-entered recompiled territory */
        return;
    }
    if (st == M68KI_OK) return;              /* interpreted to a clean halt/stop */
    fprintf(stderr, "[top-resume] interp halt(%d) at $%06X: opcode $%04X at $%06X\n",
            st, addr, g_m68ki_bad_op, g_m68ki_bad_pc);
    debug_dump_fault_trail("top-resume interp halt");
}

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

/* ---- Context-switch redirect (MC-CDI-012) ----
 * When a recompiled callee's subtree rewrites its stacked return address (the
 * OS-9 dispatcher resuming a different process at a saved PC), the JSR site that
 * detects it can't just continue at its static continuation — that belongs to
 * the OUTGOING context. Unlike g_rte_pending (which the immediate caller clears
 * to unwind one level), g_redirect_pending propagates up through EVERY C frame
 * uncleared, until the top-level trampoline in main() re-dispatches g_cpu.PC at
 * C-stack depth ~0. This is the faithful model the flat-call scheme lacks: a
 * context switch abandons the outgoing process's C frames rather than returning
 * through them. */
int      g_redirect_pending = 0;
uint32_t g_redirect_addr    = 0;

/* Set when the CPU executes STOP (shell idle waits here for an interrupt). The
 * top-level trampoline stops following the guest stack once halted; MC-CDI-007
 * will resume it on an IRQ above the SR I-mask. */
int g_halted = 0;

/* Faulting opcode for the bus/address-error frame's IRC/IR (see cdi_runtime.h). */
uint16_t g_fault_opcode = 0;
/* Faulting DATA address for the bus/address-error frame's TPF (see cdi_runtime.h). */
uint32_t g_fault_addr = 0;

/* Initial supervisor stack pointer, captured at boot (main.c). recomp_push_return
 * clamps A7 back to this when the flat-call model lets the guest stack drift
 * above its base (mirrors segagenesisrecomp's g_game_layout.initial_ssp). */
uint32_t g_recomp_initial_ssp = 0;

/* ---- Interrupt request state ----
 * Devices assert IRQ lines here. Actual delivery (vectoring into the recompiled
 * CPU between instructions) is a later milestone (MC-CDI-007/010); recording the
 * pending request lets polling boots proceed without it. */
uint32_t g_irq_pending = 0;
void cdi_irq_raise(uint8_t level) { g_irq_pending |= (1u << level); }

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

/* Generated dispatch calls this before logging a miss. We treat a miss as a
 * hybrid-interpreter handoff: the target was reached via a recomp JSR/exception
 * that left a return address on the guest stack, so interpret from there. */
int game_dispatch_override(uint32_t addr) {
    return hybrid_enter(addr, m68k_read32(g_cpu.A[7]));
}

/* JSR/BSR return-address push onto the guest supervisor stack. The flat-call
 * model returns control via the C stack (RTS = C `return`), so this exists to
 * keep the guest A7 frame consistent for code that inspects or pops the return
 * address. Mirrors segagenesisrecomp glue.c. */
void recomp_push_return(uint32_t ret_addr) {
    if (g_recomp_initial_ssp && g_cpu.A[7] > g_recomp_initial_ssp)
        g_cpu.A[7] = g_recomp_initial_ssp;
    g_cpu.A[7] -= 4;
    m68k_write32(g_cpu.A[7], ret_addr & 0xFFFFFFu);
}

/* ---- Runtime init ---- */
void runtime_init(void) {
    memset(&g_cpu, 0, sizeof g_cpu);
    /* TODO MC-CDI-001: load CD-RTOS system ROM, run the OS-9 module loader to
     * relocate the boot module (cdi_hotel) into RAM, then seed g_cpu.A[7]/PC
     * from its execution entry. Until then there is no valid start state. */
}

/* ---- Frame pacing / interrupts (need a running CD-RTOS) ---- */
/* Emitted at each instruction's END for cycle accounting. We drain the
 * accumulated cycles into the MCD212 display-timing model so its DA bit (which
 * the boot polls) advances with execution. Trace sampling lives at instruction
 * ENTRY (debug_trace_block), so this is purely the cycle/VSYNC checkpoint. */
void glue_check_vblank(void) {
    mcd212_tick(g_cycle_accumulator);
    g_cycle_accumulator = 0;
}
void glue_yield_for_vblank(void)        { /* TODO MC-CDI-007: fiber yield for frame pacing */ }
void glue_yield_for_interrupt_poll(void){ /* TODO MC-CDI-007 */ }
void runtime_request_vblank(void)       { /* TODO MC-CDI-007 */ }

/* ---- Privileged / exception semantics ---- */
void genesis_reset_devices(void) {
    /* RESET instruction: re-initialise MCD212 / CDIC / SLAVE. TODO MC-CDI-008. */
}
void genesis_stop_until_interrupt(uint16_t sr_imm) {
    g_cpu.SR = sr_imm;
    g_halted = 1;   /* the top-level trampoline stops here; MC-CDI-007 will
                     * wake on an IRQ above the I-mask and clear this. */
    /* TODO MC-CDI-007: halt until an IRQ above the I-mask; yield to pacing. */
}
/* SCC68070 exception processing (faithful port of CeDImu ProcessException):
 * enter supervisor, push the exception stack frame, then point PC at the
 * (RAM-resident) handler the kernel installed in the vector table at boot.
 * Returns the handler address; does NOT dispatch it. */
static uint32_t build_exception_frame(uint8_t vec) {
    /* TPF = CeDImu's lastAddress, captured BEFORE our own frame pushes (which go
     * through m68k_write and would clobber g_last_access_addr). For a bus error
     * this is the unmapped address the faulting access set; for the boot's odd-
     * JMP address error it is the last data EA before the jump (NOT the odd
     * target — CeDImu's GetWord throws AddressError without updating lastAddress). */
    uint32_t tpf = g_last_access_addr;
    uint16_t sr = g_cpu.SR;
    g_cpu.SR |= SR_S;

    if (vec == 2 || vec == 3) {   /* bus error / address error → long (format $F) frame */
        g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], 0);            /* internal information */
        g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], g_fault_opcode); /* IRC = faulting opcode */
        g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], g_fault_opcode); /* IR  = faulting opcode */
        g_cpu.A[7] -= 4; m68k_write32(g_cpu.A[7], 0);            /* DBIN */
        g_cpu.A[7] -= 4; m68k_write32(g_cpu.A[7], tpf);         /* TPF = lastAddress */
        g_cpu.A[7] -= 4; m68k_write32(g_cpu.A[7], 0);            /* TPD */
        g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], 0);            /* internal */
        g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], 0);            /* internal */
        g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], 0);            /* Current Move Multiple Mask */
        g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], 0);            /* Special Status Word */
        g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], (uint16_t)(0xF000u | ((uint16_t)vec << 2)));
    } else {                      /* short (format 0) frame */
        g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], (uint16_t)((uint16_t)vec << 2));
    }
    g_cpu.A[7] -= 4; m68k_write32(g_cpu.A[7], g_cpu.PC);         /* push PC (post-fetch) */
    g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], sr);              /* push SR */

    uint32_t handler = m68k_read32((uint32_t)vec << 2);
    g_cpu.PC = handler;
    return handler;
}

void m68k_raise_exception_frame(uint8_t vec) {
    build_exception_frame(vec);   /* caller (the interpreter) resumes at the handler */
}

/* ---- Recompiled-tier bus error (MC-CDI-004, recomp half) ----
 * The faulting access in cdi_bus.c has already set g_last_access_addr to the
 * unmapped address (TPF). We cannot return into the half-executed recompiled C
 * function, so build the vector-2 frame here and longjmp to the trampoline's
 * landing pad, which dispatches the handler g_cpu.PC now points at. Disarm first
 * so a fault while building the frame (e.g. a stray push) fails loud rather than
 * recursing — main() re-arms in the setjmp landing. See cdi_runtime.h. */
jmp_buf g_recomp_bus_env;
int     g_recomp_bus_armed = 0;

int recomp_bus_error(uint32_t addr) {
    if (!g_recomp_bus_armed) return 0;   /* no recomp landing pad: fail loud upstream */
    g_recomp_bus_armed = 0;

    uint32_t tpf = g_last_access_addr;    /* the unmapped address (TPF), set by the accessor */

    /* CeDImu stacks the post-fetch PC and currentOpcode. g_cpu.PC holds the
     * faulting instruction's address (set at its entry in the generated code);
     * decode it to recover its length and opcode. If undecodable, stack the
     * instruction address itself rather than fabricate a length. */
    M68KInstr ins;
    int len = m68k_interp_decode_at(g_cpu.PC, &ins);
    g_fault_opcode = len ? ins.words[0] : (uint16_t)0;
    uint32_t post_fetch = (g_cpu.PC + (uint32_t)(len ? len : 0)) & 0xFFFFFFu;

    g_last_access_addr = tpf;             /* restore TPF (decode read through the bus) */
    g_cpu.PC = post_fetch;                /* stacked PC = post-fetch PC */
    build_exception_frame(2);             /* long frame; sets g_cpu.PC = handler */
    mcd212_tick(158);                     /* CeDImu ProcessException: BusError = 158 clock periods */
    s_pending_fallback_reason = FB_BUS_HANDLER;  /* the handler runs in the interpreter next */
    longjmp(g_recomp_bus_env, 1);
    return 1;                             /* unreachable */
}

void m68k_trap_vector(uint8_t vec) {
    if (vec == 0x20) {            /* TRAP #0 = OS-9 gateway. TODO: route through the
                                   * vector table to the recompiled kernel like every
                                   * other vector and drop this HLE vestige. */
        cdrtos_syscall();
        return;
    }

    uint32_t handler = build_exception_frame(vec);
    call_by_address(handler);     /* recompiled handler, or dispatch-miss → RAM-built stub
                                   * needs the hybrid interpreter (MC-CDI-011) */
}
void m68k_illegal_trap(uint32_t pc, uint16_t opcode) {
    fprintf(stderr, "[trap] ILLEGAL/A-line/F-line opcode 0x%04X at PC=$%08X\n", opcode, pc);
    debug_dump_fault_trail("illegal opcode");
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
    debug_dump_fault_trail("movec read");
    abort();
}
void m68k_movec_write(uint16_t cc, uint32_t val) {
    fprintf(stderr, "[movec] write 0x%08X to unmodelled control register 0x%03X at PC=$%08X (TODO MC-CDI-006)\n",
            val, cc, g_cpu.PC);
    debug_dump_fault_trail("movec write");
    abort();
}

/* ---- Interpreter fallbacks for unresolved dynamic control flow (MC-CDI-011) ----
 * Emitted by generated code at computed JMP sites and unresolved calls. Both
 * route through the same clean-room interpreter handoff. JMP has no fresh
 * return push (the enclosing function's return is already on the stack); a
 * call's return address is at [A7]. */
void hybrid_jmp_interpret(uint32_t target_pc) {
    hybrid_enter(target_pc, m68k_read32(g_cpu.A[7]));
}
void hybrid_call_interpret(uint32_t target_pc) {
    hybrid_enter(target_pc, m68k_read32(g_cpu.A[7]));
}

/* ---- misc ---- */
void log_on_change(const char *label, uint32_t value) {
    static uint32_t last = 0; static const char *last_label = NULL;
    if (label != last_label || value != last) {
        fprintf(stderr, "[log] %s = 0x%08X\n", label, value);
        last = value; last_label = label;
    }
}
