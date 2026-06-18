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

/* ====================================================================== */
/*  CPU state — SCC68070 (68000-compatible programming model)             */
/* ====================================================================== */
typedef struct {
    uint32_t D[8];   /* D0-D7 data registers */
    uint32_t A[8];   /* A0-A6 address registers, A7 = SSP (supervisor stack) */
    uint16_t SR;     /* Status register: T,S,I2,I1,I0,X,N,Z,V,C */
    uint32_t PC;     /* Program counter (dynamic dispatch / debug) */
    uint32_t USP;    /* User Stack Pointer (shadow, separate from A7) */
} M68KState;

extern M68KState g_cpu;

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
/*  Addresses verified against CeDImu src/CDI/boards/Mono2/Bus.cpp.       */
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

/* Interpreter fallbacks for unresolved dynamic control flow. */
void hybrid_jmp_interpret(uint32_t target_pc);
void hybrid_call_interpret(uint32_t target_pc);

/* ====================================================================== */
/*  Exceptions / privileged control (real 68000 semantics)                */
/* ====================================================================== */
void m68k_trap_vector(uint8_t vec);              /* TRAP #N, TRAPV, CHK, ILLEGAL, A/F-line */
/* Opcode of the instruction that faulted, for the bus/address-error frame's
 * IRC/IR fields (CeDImu stacks `currentOpcode` there; the OS-9 handler reads
 * it). The generator sets this right before raising an address error. */
extern uint16_t g_fault_opcode;
void m68k_illegal_trap(uint32_t pc, uint16_t opcode);
void genesis_reset_devices(void);                /* RESET instruction */
void genesis_stop_until_interrupt(uint16_t sr_imm); /* STOP #imm */

/* On CD-i, TRAP #0 is the OS-9 / CD-RTOS system-call gateway. The trap
 * dispatcher routes vector 0x20 here; D0/D1 select F$/I$ service. */
void cdrtos_syscall(void);

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

/* ====================================================================== */
/*  Device layer (replaces the Genesis VDP/Z80/FM interface)              */
/* ====================================================================== */
/* MCD212 — Video & System Display controller (two planes, DYUV/RGB/CLUT). */
void     mcd212_write(uint32_t addr, uint32_t val, int size);
uint32_t mcd212_read (uint32_t addr, int size);
void     mcd212_render_frame(uint32_t *framebuf);   /* 384x280 (PAL) ARGB8888 */
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
void     slave_increment_frame(void);   /* fire 2-frame-delayed responses (pacing loop) */

/* A CD-i device asserted a CPU interrupt at `level` (1-7). The interrupt
 * DELIVERY model (vectoring into the recompiled CPU) is a later milestone;
 * for now this records the pending request so polling boots proceed. */
void     cdi_irq_raise(uint8_t level);
extern uint32_t g_irq_pending;           /* bitmask of pending IRQ levels */

/* SCC68070 on-chip peripherals ($80001001..$80008080): I2C, UART, timers,
 * 2 DMA channels, MMU, peripheral interrupt-control registers. */
void     periph_write(uint32_t addr, uint32_t val, int size);
uint32_t periph_read (uint32_t addr, int size);
void     periph_reset(void);             /* power-on state (UART TxRDY, etc.) */

/* ====================================================================== */
/*  ABI globals the generator references (mirrors genesis_runtime.h)      */
/* ====================================================================== */
extern uint64_t g_frame_count;
extern uint64_t g_native_insn_count;
extern uint32_t g_cycle_accumulator;     /* 68070 cycles since frame start */
extern uint32_t g_vblank_threshold;
extern uint32_t g_audio_cycle_counter;
void glue_check_vblank(void);

/* RTE / early-return propagation (see genesis_runtime.h rationale). */
extern int *g_rte_pending_ptr;
#define g_rte_pending (*g_rte_pending_ptr)
extern int g_early_return;

/* Dispatch-miss monitor. */
extern uint32_t g_miss_count_any;
extern uint32_t g_miss_last_addr;
extern uint64_t g_miss_last_frame;
#define CDI_MAX_MISS_UNIQUE 64
extern uint32_t g_miss_unique_addrs[CDI_MAX_MISS_UNIQUE];
extern int      g_miss_unique_count;

/* Change-logger helper. */
void log_on_change(const char *label, uint32_t value);
