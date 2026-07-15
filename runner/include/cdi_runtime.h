/*
 * cdi_runtime.h — Philips CD-i (SCC68070) runtime interface.
 *
 * This is the CONTRACT between the generated C (emitted by CdiRecomp's copied
 * 68000 frontend) and the hand-written runner. Generated TUs #include this;
 * the runner implements it. It mirrors segagenesisrecomp's genesis_runtime.h
 * because we share that frontend — the CPU-state ABI is identical (same 68000
 * core). What changes for CD-i is the memory map and the device layer.
 *
 * INHERITED NAMES: a few hooks the generator emits still carry the genesis_
 * prefix (genesis_stop_until_interrupt, genesis_reset_devices,
 * genesis_log_dispatch_miss). They are implemented in the runner under those
 * names for now; neutralising them to platform-agnostic names is part of the
 * shared-m68k-module extraction (TODO.md MC-CDI-009).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include "mcd212_video.h"

/* ====================================================================== */
/*  CPU state — SCC68070 (68000-compatible programming model)             */
/* ====================================================================== */
typedef struct {
    uint32_t D[8];   /* D0-D7 data registers */
    uint32_t A[8];   /* A0-A6 address registers, A7 = SSP (supervisor stack) */
    uint16_t SR;     /* Status register: T,S,I2,I1,I0,X,N,Z,V,C */
    uint32_t PC;     /* Program counter (dynamic dispatch / debug) */
    uint32_t USP;    /* inactive User Stack Pointer (holds the user A7 while S=1) */
    uint32_t SSP;    /* inactive Supervisor Stack Pointer (holds the super A7 while S=0) */
} M68KState;

extern M68KState g_cpu;

/* Write the full 16-bit SR, swapping A7 with the inactive stack pointer when the
 * S bit changes (68000: A7 aliases SSP when S=1, USP when S=0). EVERY full-SR
 * write — RTE, MOVE/ANDI/ORI/EORI to SR, STOP, and exception entry — MUST go
 * through this, or a supervisor->user transition (e.g. OS-9 dispatching a user
 * task via RTE) leaves A7 pointing at the supervisor stack and the task runs on
 * the wrong stack (corrupt returns). CCR-only flag updates do NOT use this (they
 * never touch S). See runtime.c. */
void m68k_set_sr(uint16_t new_sr);

/* SR flag bits (identical to 68000) */
#define SR_C   (1u << 0)
#define SR_V   (1u << 1)
#define SR_Z   (1u << 2)
#define SR_N   (1u << 3)
#define SR_X   (1u << 4)
#define SR_I0  (1u << 8)
#define SR_I1  (1u << 9)
#define SR_I2  (1u << 10)
#define SR_S   (1u << 13)
#define SR_T   (1u << 15)

/* ====================================================================== */
/*  CD-i memory map (Mono-I/II class player)                              */
/*  Physical addresses follow the Mono board map and cdi490 ROM probes.   */
/*  The SCC68070 has an on-chip MMU; CD-RTOS may remap — these are the    */
/*  power-on physical decodes the bus uses by default.                    */
/* ====================================================================== */
#define CDI_RAM0_BASE   0x00000000u   /* 512 KB system RAM, bank 0            */
#define CDI_RAM0_SIZE   0x00080000u
#define CDI_RAM1_BASE   0x00200000u   /* 512 KB system RAM, bank 1 (1 MB tot) */
#define CDI_RAM1_SIZE   0x00080000u
#define CDI_CDIC_BASE   0x00300000u   /* CDIC: CD interface + ADPCM audio (board-dependent) */
#define CDI_SLAVE_BASE  0x00310000u   /* SLAVE i8051: pointer/input, on odd bytes (..0x31001E) */
#define CDI_NVRAM_BASE  0x00320000u   /* Timekeeper RTC + battery NVRAM, on even bytes */
#define CDI_ROM_BASE    0x00400000u   /* CD-RTOS system ROM (~0x400000..0x4FFFDF) */
#define CDI_ROM_SIZE    0x00100000u
#define CDI_MCD212_BASE 0x004FFFE0u   /* MCD212 internal registers (top of ROM window) */
#define CDI_PERIPH_BASE 0x80001001u   /* SCC68070 on-chip peripherals (I2C/UART/timers/DMA/MMU) */
#define CDI_PERIPH_LAST 0x80008080u

/* ====================================================================== */
/*  Memory interface — the ONLY way generated code touches memory.        */
/*  All loads/stores route through these; the CD-i memory model (RAM      */
/*  banks, ROM, MMIO, MMU translation) lives entirely behind them.        */
/* ====================================================================== */
uint8_t  m68k_read8 (uint32_t addr);
uint16_t m68k_read16(uint32_t addr);
uint32_t m68k_read32(uint32_t addr);
void     m68k_write8 (uint32_t addr, uint8_t  val);
void     m68k_write16(uint32_t addr, uint16_t val);
void     m68k_write32(uint32_t addr, uint32_t val);

/* ====================================================================== */
/*  Dispatch + trampolines                                                */
/*  recomp_* and call_by_address are DEFINED in the generated dispatch    */
/*  TU; declared here so the runner and generated code agree on sigs.     */
/* ====================================================================== */
typedef void (*RecompFuncPtr)(void);
void call_by_address(uint32_t addr);          /* JMP (An) / jump tables       */
void recomp_tail_call(uint32_t addr);
void recomp_call_addr(uint32_t addr);
/* Depth-0 resume used by the top-level trampoline. A recompiled-entry target is
 * dispatched flat-call; a non-entry target is interpreted until it re-enters a
 * recompiled entry, WITHOUT the ret-stop game_dispatch_override applies for
 * nested JSR misses — at depth 0 there is no C caller to return to, so a hybrid
 * RTS that pops the next return must FLOW into it rather than hand a consumed
 * guest stack back to the loop (which would double-follow [A7]). DEFINED in the
 * runner (runtime.c), not the generated TU. */
void recomp_top_resume(uint32_t addr);
void recomp_call_func(RecompFuncPtr fn);
void recomp_push_return(uint32_t ret_addr);

/* Initial supervisor stack pointer captured at boot; recomp_push_return clamps
 * the guest A7 to this to bound flat-call-model drift. */
extern uint32_t g_recomp_initial_ssp;

/* Runtime-provided: logged when call_by_address finds no generated function. */
void genesis_log_dispatch_miss(uint32_t addr);

/* Always-on execution trace tap. The generator emits a call to this at the
 * ENTRY of every instruction (right after the guest PC store), so the trace ring
 * captures one in-order sample per instruction — control-transfer instructions
 * (JSR/BSR) included, BEFORE they transfer control. Implemented in
 * debug_server.c. (Capturing at instruction END instead would miss the JSR
 * sample, since the call dives into the callee before reaching the hook.) */
void debug_trace_block(void);
void debug_record_irq_raise(uint8_t level);   /* always-on IRQ-raise ring (MC-CDI-007 boundary diff) */

/* Interpreter fallbacks for unresolved dynamic control flow. */
void hybrid_jmp_interpret(uint32_t target_pc);
void hybrid_call_interpret(uint32_t target_pc);

/* ====================================================================== */
/*  Exceptions / privileged control (real 68000 semantics)                */
/* ====================================================================== */
void m68k_trap_vector(uint8_t vec);              /* TRAP #N, TRAPV, CHK, ILLEGAL, A/F-line */
/* Build the exception stack frame for `vec` and point PC at the handler, but do
 * NOT dispatch it (no recursive call_by_address). Used by the interpreter, which
 * raises a bus/address error mid-instruction and then continues its own loop
 * into the RAM-resident handler without recursive host calls. */
void m68k_raise_exception_frame(uint8_t vec);
/* Opcode of the instruction that faulted, for the SCC68070 long frame's IRC/IR
 * fields. The generator sets this right before raising an address error. */
extern uint16_t g_fault_opcode;
/* Faulting data address for the bus/address-error frame's TPF field. For a bus
 * error this is the unmapped address the
 * instruction tried to touch; for the boot's deliberate JMP-to-odd address
 * error it is the odd target. Distinct from the stacked PC (which is the
 * instruction's post-fetch PC). */
extern uint32_t g_fault_addr;
/* Last operand effective address accessed. The bus/address-error long frame
 * stacks this as TPF. Set by every m68k_read/write
 * in cdi_bus.c; captured at exception entry (build_exception_frame) before our
 * own frame pushes overwrite it. For an ADDRESS error this is the last data EA
 * before the faulting odd jump (e.g. $000510), NOT the odd target. */
extern uint32_t g_last_access_addr;
/* Bus-error hook (cdi_bus.c -> interpreter). When the interpreter is executing
 * an instruction and a memory access lands on an unmapped address, the access
 * routes here. If the interpreter has armed its mid-instruction unwind, this
 * records the faulting address and longjmps out to raise a faithful SCC68070
 * bus error (vector 2); it never returns in that case. Returns 0 when no
 * interpreter unwind is armed, so the caller falls through to fail-loud (a
 * genuinely unmodelled region reached from recompiled code). */
int m68k_interp_bus_error(uint32_t addr);

/* ---- Recompiled-tier bus-error unwind (MC-CDI-004, recomp half) ----
 * The interpreter owns its instruction loop and arms a longjmp around exec_one
 * (m68k_interp_bus_error above). The recompiled tier reaches memory through
 * plain C calls we cannot unwind in place, so instead the top-level trampoline
 * in main() arms a landing pad with setjmp(g_recomp_bus_env) and sets
 * g_recomp_bus_armed. When a fault from recompiled code reaches cdi_bus.c's
 * fault path and the interpreter is NOT armed, recomp_bus_error() builds the
 * faithful vector-2 frame (post-fetch PC, faulting opcode, TPF = unmapped
 * address), points PC at the OS-9 handler, and longjmps back to the trampoline,
 * which dispatches the handler. Abandoning the half-executed recompiled C frame
 * IS the faithful abort of the faulting instruction. Returns 0 when no recomp
 * landing pad is armed, so the caller falls through to fail-loud. */
extern jmp_buf g_recomp_bus_env;
extern int     g_recomp_bus_armed;
int  recomp_bus_error(uint32_t addr);

/* ---- Level-triggered external-interrupt delivery (MC-CDI-007) ----
 * A device raises an autovectored interrupt via cdi_irq_raise(level), which sets
 * a bit in g_irq_pending. The DA-poll spin the boot blocks in is RECOMPILED (a
 * tight goto loop inside func_41BEE2) — the top-level trampoline can't preempt it
 * and we must NOT run the OS-9 ISR nested in that function's C frame (it fights
 * OS-9 task switching). So delivery mirrors the recomp bus-error unwind: the
 * per-instruction ENTRY safepoint (debug_trace_block, in BOTH tiers) calls
 * recomp_take_irq() at an instruction boundary; if an unmasked interrupt is
 * pending (level 7, or level > SR IPM) it builds the autovector frame (short
 * frame, stacked PC = g_cpu.PC = the not-yet-executed instruction), consumes
 * the pending edge, points PC at the handler from the
 * vector table, and longjmps to the trampoline's IRQ landing pad, which
 * dispatches it. Returns without effect when disarmed or nothing is deliverable. */
extern jmp_buf g_recomp_irq_env;
extern int     g_recomp_irq_armed;
int  recomp_pending_irq_level(void);   /* highest pending unmasked-agnostic level, or 0 */
uint32_t recomp_pending_irq_mask(void);
void recomp_take_irq(void);            /* deliver at an instruction boundary (may longjmp) */

void m68k_illegal_trap(uint32_t pc, uint16_t opcode);
void genesis_reset_devices(void);                /* RESET instruction */
void genesis_stop_until_interrupt(uint16_t sr_imm); /* STOP #imm */

/* MOVEC control-register access (68010/SCC68070). The runtime owns the
 * SCC68070 control-register model; generated code passes the 12-bit control
 * code (Cc) and the long value. Not modelled yet — these FAIL LOUD with the
 * exact code/PC (the ROM's MOVEC sites are dead code on the SCC68070, gated
 * behind a CPU-type dispatch, so they aren't expected to fire). */
uint32_t m68k_movec_read (uint16_t cc);
void     m68k_movec_write(uint16_t cc, uint32_t val);

/* ====================================================================== */
/*  Fiber yields / frame pacing                                           */
/* ====================================================================== */
void glue_yield_for_vblank(void);
void glue_yield_for_interrupt_poll(void);
void runtime_init(void);
void runtime_request_vblank(void);
/* Pace player-mode CPU/device advancement against host monotonic time. The
 * emulated clock remains cycle-derived; fixed-sequence co-sim disables this so
 * deterministic audits free-run. */
void runtime_set_realtime_pacing(int enabled);
void runtime_pace_cycles(uint32_t cycles);

/* ====================================================================== */
/*  Device layer (replaces the Genesis VDP/Z80/FM interface)              */
/* ====================================================================== */
/* MCD212 — Video & System Display controller (two planes, DYUV/RGB/CLUT). */
void     mcd212_write(uint32_t addr, uint32_t val, int size);
uint32_t mcd212_read (uint32_t addr, int size);
/* Copy the last completed canonical ARGB8888 frame (up to 768x560). */
void     mcd212_render_frame(uint32_t *framebuf);
uint32_t mcd212_framebuffer_info(uint16_t *width, uint16_t *height,
                                 uint64_t *generation);
uint64_t mcd212_framebuffer_hash(uint16_t *width, uint16_t *height,
                                 uint64_t *generation);
void     mcd212_debug_state(uint16_t regs[32], uint8_t *csr1r,
                            uint8_t *csr2r, uint32_t *vline,
                            uint16_t *active_line);
/* Advance MCD212 display timing by `cycles` of CPU time. Drives the vertical
 * line counter and the CSR1R DA (Display Active) bit the boot polls. Called per
 * instruction from both execution tiers (glue_check_vblank + the interpreter). */
void     mcd212_tick(uint32_t cycles);

/* CDIC — CD Interface Controller: sector delivery + ADPCM audio decode. */
void     cdic_write(uint32_t addr, uint32_t val, int size);
uint32_t cdic_read (uint32_t addr, int size);

/* IKAT — input/serial-gate MCU (Mono-2/3/4): pointer/controller input,
 * front-panel, and the boot-time command/response gate (IKAT version, boot
 * mode, video standard, disc status). "slave" naming is Mono-1/2 legacy. */
void     slave_write(uint32_t addr, uint32_t val, int size);
uint32_t slave_read (uint32_t addr, int size);
/* Advance IKAT on the board's emulated-time edge. This drives both its 25 ms
 * maneuvering-device packet cadence and its frame-delayed disc
 * responses. `ns` is derived from SCC68070 cycles, never host wall-clock. */
void     slave_increment_time(double ns);

/* Host pointing-device state, consumed by slave_increment_time(). Bits are
 * deliberately a transport-neutral runtime ABI: SDL and the TCP debug surface
 * feed the same IKAT path rather than either one poking guest memory. */
enum {
    CDI_INPUT_LEFT  = 1u << 0,
    CDI_INPUT_UP    = 1u << 1,
    CDI_INPUT_RIGHT = 1u << 2,
    CDI_INPUT_DOWN  = 1u << 3,
    CDI_INPUT_BTN1  = 1u << 4,
    CDI_INPUT_BTN2  = 1u << 5
};
void     cdi_input_set(uint32_t mask);
uint32_t cdi_input_get(void);
void     cdi_input_reset(void);
void     cdi_input_add_relative(int x, int y);
void     cdi_input_clear_relative(void);
void     cdi_input_take_relative(int *x, int *y,
                                 int min_x, int max_x,
                                 int min_y, int max_y);
void     cdi_input_pending_relative(int *x, int *y);
void     cdi_input_mouse_configure(int enabled);
void     cdi_input_mouse_focus(int focused);
int      cdi_input_mouse_active(void);
void     cdi_input_mouse_motion(int x, int y);
void     cdi_input_mouse_button(int button, int pressed);
void     cdi_input_acknowledge_mouse_buttons(void);
/* Side-effect-free snapshot for the always-on debug surface. Racy but
 * monotonic enough for observation; unlike slave_read(), it never consumes a
 * response byte or clears device state. */
void     slave_debug_state(uint8_t regs[15], uint8_t out_remaining[4],
                           double *pointer_time_ns, int *cursor_packets);
typedef struct {
    uint64_t seq, trace_seq, frame, cycles;
    uint32_t pc;
    uint8_t  type, channel, length, data[8];
} CdiIkatEvent;
enum {
    CDI_IKAT_COMMAND = 1,
    CDI_IKAT_RESPONSE = 2,
    CDI_IKAT_MEDIA = 3,
    CDI_IKAT_IRQ = 4,      /* mask-approved external level-2 assertion */
    CDI_IKAT_READ = 5      /* guest consumed one output byte */
};
int      slave_debug_events(CdiIkatEvent *out, int capacity, uint64_t *total);

/* A CD-i device asserts a CPU interrupt at `level` (1-7). Delivery occurs at
 * the universal instruction-entry safepoint, or directly from the STOP device
 * loop, and respects the SR interrupt mask. */
void     cdi_irq_raise(uint8_t level);
void     cdi_irq_raise_onchip(uint8_t input);
void     cdi_irq_raise_onchip_level(uint8_t level);
extern uint32_t g_irq_pending;           /* bitmask of pending IRQ levels */

/* SCC68070 on-chip peripherals ($80001001..$80008080): I2C, UART, timers,
 * 2 DMA channels, MMU, peripheral interrupt-control registers. */
void     periph_write(uint32_t addr, uint32_t val, int size);
uint32_t periph_read (uint32_t addr, int size);
void     periph_reset(void);             /* power-on state (UART TxRDY, etc.) */
uint8_t  periph_lir(void);                /* side-effect-free LIR snapshot */
void     periph_increment_timer(uint32_t cycles);

/* DS1216 SmartWatch timekeeper + 32 KB NVRAM at $320000. Per the DS1216 data
 * sheet, SRAM access continues until the 64-bit D0 comparison pattern unlocks
 * a serial RTC transfer. `dev` is (busaddr-$320000)>>1. Reset uses the
 * deterministic 1989-01-01 test epoch. */
void    nvram_reset(void);
/* Player-only battery SRAM persistence. The clock remains independent: these
 * functions load/save exactly the 32 KiB SRAM and never alter RTC state. Load
 * returns 1 on success, 0 when no file exists, and -1 for invalid/unreadable
 * input. Save uses a same-directory temporary file and atomic replacement. */
int     nvram_load_sram(const char *path);
int     nvram_save_sram(const char *path);
typedef struct CdiRtcTime {
    int year;              /* representable startup range: 1970..2069 */
    uint8_t month;         /* 1..12 */
    uint8_t date;          /* 1..31, validated for month/year */
    uint8_t weekday;       /* 1=Monday .. 7=Sunday */
    uint8_t hour;          /* 0..23 */
    uint8_t minute;
    uint8_t second;
    uint8_t hundredth;
} CdiRtcTime;
/* Returns 1 when applied, 0 if startup was already seeded, -1 if invalid. */
int     nvram_seed_clock_once(const CdiRtcTime *time_value);
uint8_t nvram_get_byte(uint16_t dev);    /* GetByte: SRAM, or one serial RTC bit */
void    nvram_set_byte(uint16_t dev, uint8_t data);
/* Advance the DS1216 by cycle-derived emulated time, never host wall time.
 * mcd212_tick() supplies the board's common device-time quantum. */
void    nvram_increment_clock(double ns);

/* ====================================================================== */
/*  ABI globals the generator references (mirrors genesis_runtime.h)      */
/* ====================================================================== */
extern uint64_t g_frame_count;
extern uint64_t g_total_cycles;   /* running SCC68070 clock (sum of mcd212_tick inputs) */
extern uint64_t g_native_insn_count;
extern uint32_t g_cycle_accumulator;     /* 68070 cycles since frame start */
extern uint32_t g_vblank_threshold;
extern uint32_t g_audio_cycle_counter;
void glue_check_vblank(void);
/* Queue exception-entry periods for the handler's first instruction. If an
 * older exception batch is pending, finish that device-time quantum first. */
void runtime_defer_exception_cycles(uint32_t cycles);

/* RTE / early-return propagation (see genesis_runtime.h rationale). */
extern int *g_rte_pending_ptr;
#define g_rte_pending (*g_rte_pending_ptr)
extern int g_early_return;

/* Set by a recompiled RTE/RTR (alongside g_rte_pending): the return instruction
 * popped its frame and g_cpu.PC now holds the resume PC. When this unwind
 * bottoms out to the top-level trampoline (the exception was entered via
 * m68k_trap_vector -> call_by_address at depth 0, so no JSR site owns it), the
 * trampoline must resume at g_cpu.PC — NOT follow [A7] like a (skip-)RTS, whose
 * g_cpu.PC is stale. Distinguishes a real frame return from the skip-RTS idiom. */
extern int g_rte_resume;

/* Context-switch redirect (MC-CDI-012). Set at a JSR site when the callee
 * subtree rewrote the stacked return address (OS-9 process switch); propagates
 * UNCLEARED up every C frame to the top-level trampoline in main(), which
 * re-dispatches g_redirect_addr. Distinct from g_rte_pending, which the
 * immediate caller clears to unwind a single level. */
extern int      g_redirect_pending;
extern uint32_t g_redirect_addr;
extern int      g_call_was_hybrid;   /* last dispatch ran hybrid (1) vs flat-call (0) */

/* Set by STOP (genesis_stop_until_interrupt). The top-level trampoline stops
 * following the guest stack once halted; MC-CDI-007 clears it on an IRQ. */
extern int g_halted;

/* Dispatch-miss monitor. */
extern uint32_t g_miss_count_any;
extern uint32_t g_miss_last_addr;
extern uint64_t g_miss_last_frame;
#define CDI_MAX_MISS_UNIQUE 64
extern uint32_t g_miss_unique_addrs[CDI_MAX_MISS_UNIQUE];
extern int      g_miss_unique_count;

/* Change-logger helper. */
void log_on_change(const char *label, uint32_t value);
