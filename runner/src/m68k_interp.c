/*
 * m68k_interp.c — Tier-3 clean-room 68000 interpreter (the floor).
 *
 * See m68k_interp.h for the design. In one line: it REUSES the recompiler's
 * decoder (m68k_decoder.c) and MIRRORS code_generator.c's per-mnemonic
 * semantics exactly, executing on the g_cpu / m68k_read*-write* runtime ABI.
 * Every EA-resolution and flag formula here is a direct "emit C" -> "execute"
 * port of the corresponding code_generator.c helper, so the interpreter and
 * the static path are parity-by-construction; clown68000 is the runtime oracle
 * that proves it (see runner/tests/m68k_interp_diff.c).
 *
 * Precision over recall: anything not implemented HALTS LOUDLY (returns
 * M68KI_HALT_UNIMPL with g_m68ki_bad_pc/op set) — it never silently
 * mis-executes. Widening coverage is purely additive and always safe.
 */
#include "m68k_interp.h"
#include "m68k_decoder.h"   /* recompiler/src — added to the runner include path */
#include "rom_parser.h"     /* GenesisRom, rom_read* (decoder's fetch source)    */
#include "debug_server.h"   /* debug_trace_interp (fallback classifier)          */

#include <stdio.h>
#include <string.h>
#include <setjmp.h>

/* ---- mid-instruction bus-error unwind (MC-CDI-004) ----
 * A memory access to an unmapped address is a real SCC68070 bus error. The
 * faulting instruction must be ABORTED (no result committed) and the exception
 * raised — exactly as CeDImu throws a C++ Exception out of GetWord. The
 * generated/recompiled tier reaches memory through plain C calls we can't
 * unwind, but the interpreter owns its instruction loop, so it arms a longjmp
 * around exec_one. cdi_bus.c's fault path calls m68k_interp_bus_error(), which
 * longjmps back here when armed. s_bus_env is saved/restored per step so nested
 * interpreter runs (e.g. the handler the bus error vectors into) don't clobber
 * an outer frame's landing pad. */
static jmp_buf s_bus_env;
static int     s_bus_armed = 0;
static uint32_t s_fault_pc = 0;   /* post-fetch PC to stack on the frame */

int m68k_interp_bus_error(uint32_t addr) {
    if (!s_bus_armed) return 0;   /* not in an armed interpreter step: fail loud upstream */
    g_fault_addr = addr;          /* TPF = the unmapped DATA address (CeDImu lastAddress) */
    s_bus_armed = 0;              /* disarm before unwinding past exec_one */
    longjmp(s_bus_env, 1);
    return 1;                     /* unreachable */
}

/* ---- per-run diagnostics ---- */
uint32_t g_m68ki_bad_pc = 0;
uint16_t g_m68ki_bad_op = 0;
uint64_t g_m68ki_insn_count = 0;

/* Coverage discovery (see header): distinct call/jump targets this run. */
uint32_t g_m68ki_discover[M68KI_MAX_DISCOVER];
int      g_m68ki_discover_count = 0;
static void discover(uint32_t t) {
    t &= 0xFFFFFFu;
    /* Accumulate every call/jump target seen during interpretation into the
     * persistent trace-guided discovery set (deduped there). These are the
     * function/block entries the static finder didn't reach — when interpreted
     * code calls into ROM that isn't a recompiled dispatch entry, the
     * interpreter keeps running instead of handing back, which is why the
     * fallback is dominated by uncovered ROM. Seeding these (collect_seeds.py →
     * cdrtos_discovered.txt → CdiRecompBios --seeds) recompiles them so the
     * interpreter hands back at their entry. Unlike g_m68ki_discover below, this
     * set is NOT reset per run. */
    debug_record_indirect_target(t);
    for (int i = 0; i < g_m68ki_discover_count; i++)
        if (g_m68ki_discover[i] == t) return;
    if (g_m68ki_discover_count < M68KI_MAX_DISCOVER)
        g_m68ki_discover[g_m68ki_discover_count++] = t;
}

/* Set when an instruction tries to write An through the general EA path. No
 * legal instruction does this (MOVEA/ADDA/LEA/ADDQ write An directly), so it
 * means an illegal encoding — only reachable when a fuzzed/garbage computed
 * jump lands on non-code bytes. The step wrapper turns it into a loud halt. */
static int s_illegal_ea = 0;

/* =========================================================================
 * Instruction fetch — CD-i diverges from the Genesis here. On the Genesis all
 * missed code lives in ROM, so a flat g_rom view suffices. On CD-i the whole
 * point of the floor is RAM-resident code: CD-RTOS builds vector stubs and
 * relocates modules into RAM, so a missed PC can be anywhere the bus decodes
 * (RAM $000000.. or ROM $400000..). We therefore fetch the decode window from
 * the BUS (m68k_read8) into a small buffer and present it to the shared decoder
 * as a GenesisRom whose backing is offset so rom_read*(view, A) returns the
 * window byte for absolute address A. The decoder needs the REAL address (it
 * computes Bcc/BSR targets as addr + disp), so the view must answer for the
 * absolute PC, not a normalised offset.
 * ========================================================================= */
#define BUSWIN 64                     /* >= max 68000 instruction length (10 B) */
static uint8_t    s_win[BUSWIN];
static GenesisRom s_busview;

static const GenesisRom *busview(uint32_t pc) {
    pc &= 0xFFFFFFu;
    for (int i = 0; i < BUSWIN; i++)
        s_win[i] = m68k_read8((pc + (uint32_t)i) & 0xFFFFFFu);
    /* Base-offset so (rom_data)[A] == s_win[A-pc] for A in [pc, pc+BUSWIN).
     * Forming s_win-pc is the standard window-view idiom; only ever indexed in
     * that range, which maps back into s_win. */
    s_busview.rom_data = s_win - pc;
    s_busview.rom_size = pc + BUSWIN;
    return &s_busview;
}

int m68k_interp_decode_at(uint32_t pc, M68KInstr *out) {
    /* busview() fetches its window through m68k_read8, which updates
     * g_last_access_addr. The recomp-tier bus-error path calls this AFTER the
     * faulting access set g_last_access_addr (the TPF) but BEFORE building the
     * frame, so preserve it across the decode. */
    uint32_t saved = g_last_access_addr;
    int ok = m68k_decode(busview(pc), pc & 0xFFFFFFu, out);
    g_last_access_addr = saved;
    return ok ? out->byte_length : 0;
}

/* =========================================================================
 * Size helpers (mirror code_generator.c)
 * ========================================================================= */
static uint32_t szmask(M68KSize sz) {
    switch (sz) { case M68K_SIZE_B: return 0xFFu;
                  case M68K_SIZE_L: return 0xFFFFFFFFu;
                  default:          return 0xFFFFu; }
}
static int szbits(M68KSize sz) {
    switch (sz) { case M68K_SIZE_B: return 8;
                  case M68K_SIZE_L: return 32;
                  default:          return 16; }
}
static int szbytes(M68KSize sz) {
    switch (sz) { case M68K_SIZE_B: return 1;
                  case M68K_SIZE_L: return 4;
                  default:          return 2; }
}
/* "keep" mask for size-preserving Dn writes (.B/.W keep upper bits). */
static uint32_t keepmask(M68KSize sz) {
    return (sz == M68K_SIZE_B) ? 0xFFFFFF00u
         : (sz == M68K_SIZE_W) ? 0xFFFF0000u : 0x00000000u;
}
static void store_dn(int reg, uint32_t val, M68KSize sz) {
    if (sz == M68K_SIZE_L) g_cpu.D[reg] = val;
    else g_cpu.D[reg] = (g_cpu.D[reg] & keepmask(sz)) | (val & szmask(sz));
}

/* =========================================================================
 * ExtReader — sequential walker over instruction extension words.
 * Direct port of code_generator.c's ExtReader (wi starts at 1, bp at 2).
 * ========================================================================= */
typedef struct { const M68KInstr *ins; int wi; uint32_t bp; } ExtR;
static void er_init(ExtR *e, const M68KInstr *ins)        { e->ins = ins; e->wi = 1;       e->bp = 2; }
static void er_init_at(ExtR *e, const M68KInstr *ins, int wi){ e->ins = ins; e->wi = wi;   e->bp = (uint32_t)(wi * 2); }
static uint16_t er_next(ExtR *e)        { uint16_t v = e->ins->words[e->wi]; e->wi++; e->bp += 2; return v; }
static uint32_t er_next_dword(ExtR *e)  { uint16_t hi = er_next(e); uint16_t lo = er_next(e); return ((uint32_t)hi << 16) | lo; }
static uint32_t er_next_imm(ExtR *e, M68KSize sz) {
    if (sz == M68K_SIZE_L) return er_next_dword(e);
    uint16_t w = er_next(e);
    return (sz == M68K_SIZE_B) ? (uint32_t)(w & 0xFFu) : (uint32_t)w;
}

/* =========================================================================
 * Effective-address resolution. read_ea_ex / addr_ea / write_ea_ex mirror
 * emit_ea_load_ex / emit_ea_addr_ex / emit_ea_store_ex exactly, including the
 * (An)+/-(An) side effects and the rmw suppression of the increment.
 *
 * read_ea_ex returns the operand "wide": Dn/An return the full 32-bit
 * register, memory returns the size-width read, immediates return the imm.
 * Callers size-mask at the point of use (matching the generated `(ct)expr`).
 * ========================================================================= */
static uint32_t mem_read(M68KSize sz, uint32_t a) {
    switch (sz) { case M68K_SIZE_B: return m68k_read8(a);
                  case M68K_SIZE_L: return m68k_read32(a);
                  default:          return m68k_read16(a); }
}
static void mem_write(M68KSize sz, uint32_t a, uint32_t v) {
    switch (sz) { case M68K_SIZE_B: m68k_write8(a, (uint8_t)v); break;
                  case M68K_SIZE_L: m68k_write32(a, v);        break;
                  default:          m68k_write16(a, (uint16_t)v); break; }
}

static uint32_t read_ea_ex(const M68KInstr *ins, int ea, M68KSize sz, ExtR *er, int rmw) {
    int mode = (ea >> 3) & 7, reg = ea & 7, sb = szbytes(sz);
    switch (mode) {
    case 0: return g_cpu.D[reg];
    case 1: return g_cpu.A[reg];
    case 2: return mem_read(sz, g_cpu.A[reg]);
    case 3: { uint32_t v = mem_read(sz, g_cpu.A[reg]); if (!rmw) g_cpu.A[reg] += sb; return v; }
    case 4: { g_cpu.A[reg] -= sb; return mem_read(sz, g_cpu.A[reg]); }
    case 5: { int16_t d16 = (int16_t)er_next(er);
              return mem_read(sz, (uint32_t)(g_cpu.A[reg] + (int32_t)d16)); }
    case 6: { uint16_t ext = er_next(er);
              int xreg = (ext >> 12) & 7, xtype = (ext >> 15) & 1; int8_t d8 = (int8_t)(ext & 0xFF);
              int32_t xv = ((ext >> 11) & 1) ? (int32_t)(xtype ? g_cpu.A[xreg] : g_cpu.D[xreg]) /* .L */ : (int32_t)(int16_t)(xtype ? g_cpu.A[xreg] : g_cpu.D[xreg]); /* .W */
              return mem_read(sz, (uint32_t)(g_cpu.A[reg] + xv + (int32_t)d8)); }
    case 7:
        switch (reg) {
        case 0: { int16_t ext = (int16_t)er_next(er); return mem_read(sz, (uint32_t)(int32_t)ext); }
        case 1: { uint32_t a = er_next_dword(er); return mem_read(sz, a); }
        case 2: { uint32_t pc = ins->addr + er->bp; int16_t d16 = (int16_t)er_next(er);
                  return mem_read(sz, (uint32_t)((int32_t)pc + (int32_t)d16)); }
        case 3: { uint32_t pc = ins->addr + er->bp; uint16_t ext = er_next(er);
                  int xreg = (ext >> 12) & 7, xtype = (ext >> 15) & 1; int8_t d8 = (int8_t)(ext & 0xFF);
                  int32_t xv = ((ext >> 11) & 1) ? (int32_t)(xtype ? g_cpu.A[xreg] : g_cpu.D[xreg]) /* .L */ : (int32_t)(int16_t)(xtype ? g_cpu.A[xreg] : g_cpu.D[xreg]); /* .W */
                  return mem_read(sz, (uint32_t)(pc + xv + (int32_t)d8)); }
        case 4: return er_next_imm(er, sz);
        default: return 0;
        }
    default: return 0;
    }
}
static uint32_t read_ea(const M68KInstr *ins, int ea, M68KSize sz, ExtR *er) {
    return read_ea_ex(ins, ea, sz, er, 0);
}

/* addr_ea — the address of a control/alterable EA (LEA/PEA/JMP/JSR). */
static uint32_t addr_ea(const M68KInstr *ins, int ea, ExtR *er) {
    int mode = (ea >> 3) & 7, reg = ea & 7;
    switch (mode) {
    case 2: return g_cpu.A[reg];
    case 5: { int16_t d16 = (int16_t)er_next(er); return (uint32_t)(g_cpu.A[reg] + (int32_t)d16); }
    case 6: { uint16_t ext = er_next(er);
              int xreg = (ext >> 12) & 7, xtype = (ext >> 15) & 1; int8_t d8 = (int8_t)(ext & 0xFF);
              int32_t xv = ((ext >> 11) & 1) ? (int32_t)(xtype ? g_cpu.A[xreg] : g_cpu.D[xreg]) /* .L */ : (int32_t)(int16_t)(xtype ? g_cpu.A[xreg] : g_cpu.D[xreg]); /* .W */
              return (uint32_t)(g_cpu.A[reg] + xv + (int32_t)d8); }
    case 7:
        switch (reg) {
        case 0: { int16_t ext = (int16_t)er_next(er); return (uint32_t)(int32_t)ext; }
        case 1: return er_next_dword(er);
        case 2: { uint32_t pc = ins->addr + er->bp; int16_t d16 = (int16_t)er_next(er);
                  return (uint32_t)((int32_t)pc + (int32_t)d16); }
        case 3: { uint32_t pc = ins->addr + er->bp; uint16_t ext = er_next(er);
                  int xreg = (ext >> 12) & 7, xtype = (ext >> 15) & 1; int8_t d8 = (int8_t)(ext & 0xFF);
                  int32_t xv = ((ext >> 11) & 1) ? (int32_t)(xtype ? g_cpu.A[xreg] : g_cpu.D[xreg]) /* .L */ : (int32_t)(int16_t)(xtype ? g_cpu.A[xreg] : g_cpu.D[xreg]); /* .W */
                  return (uint32_t)(pc + xv + (int32_t)d8); }
        default: return 0;
        }
    default: return 0;
    }
}

static void write_ea_ex(const M68KInstr *ins, int ea, M68KSize sz, ExtR *er, uint32_t val, int rmw) {
    int mode = (ea >> 3) & 7, reg = ea & 7, sb = szbytes(sz);
    (void)ins;
    switch (mode) {
    case 0: store_dn(reg, val, sz); break;
    case 1: s_illegal_ea = 1; break;          /* An is never a legal write_ea target */
    case 2: mem_write(sz, g_cpu.A[reg], val); break;
    case 3: mem_write(sz, g_cpu.A[reg], val); g_cpu.A[reg] += sb; break;
    case 4: if (!rmw) g_cpu.A[reg] -= sb; mem_write(sz, g_cpu.A[reg], val); break;
    case 5: { int16_t d16 = (int16_t)er_next(er);
              mem_write(sz, (uint32_t)(g_cpu.A[reg] + (int32_t)d16), val); break; }
    case 6: { uint16_t ext = er_next(er);
              int xreg = (ext >> 12) & 7, xtype = (ext >> 15) & 1; int8_t d8 = (int8_t)(ext & 0xFF);
              int32_t xv = ((ext >> 11) & 1) ? (int32_t)(xtype ? g_cpu.A[xreg] : g_cpu.D[xreg]) /* .L */ : (int32_t)(int16_t)(xtype ? g_cpu.A[xreg] : g_cpu.D[xreg]); /* .W */
              mem_write(sz, (uint32_t)(g_cpu.A[reg] + xv + (int32_t)d8), val); break; }
    case 7:
        switch (reg) {
        case 0: { int16_t ext = (int16_t)er_next(er); mem_write(sz, (uint32_t)(int32_t)ext, val); break; }
        case 1: { uint32_t a = er_next_dword(er); mem_write(sz, a, val); break; }
        default: break; /* PC-relative/imm are not alterable destinations */
        }
        break;
    default: break;
    }
}
static void write_ea(const M68KInstr *ins, int ea, M68KSize sz, ExtR *er, uint32_t val) {
    write_ea_ex(ins, ea, sz, er, val, 0);
}

/* =========================================================================
 * Flag helpers — exact ports of emit_flags_{logic,add,sub,cmp}.
 * (X=bit4 N=bit3 Z=bit2 V=bit1 C=bit0.)
 * ========================================================================= */
static void flags_logic(uint32_t v, M68KSize sz) {
    int bits = szbits(sz); uint32_t fv = v & szmask(sz);
    g_cpu.SR &= ~0x0Fu;
    if (!fv)                 g_cpu.SR |= SR_Z;
    if (fv >> (bits - 1))    g_cpu.SR |= SR_N;
}
static void flags_add(uint32_t a, uint32_t b, uint32_t r, M68KSize sz) {
    int bits = szbits(sz); uint32_t m = szmask(sz), sb = (uint32_t)(1u << (bits - 1));
    uint32_t fa = a & m, fb = b & m, fr = r & m;
    g_cpu.SR &= ~0x1Fu;
    if (!fr)                                   g_cpu.SR |= SR_Z;
    if (fr >> (bits - 1))                      g_cpu.SR |= SR_N;
    if ((uint64_t)fa + (uint64_t)fb > m)     { g_cpu.SR |= SR_C; g_cpu.SR |= SR_X; }
    if (!((fa ^ fb) & sb) && ((fa ^ fr) & sb)) g_cpu.SR |= SR_V;
}
static void flags_sub(uint32_t a, uint32_t b, uint32_t r, M68KSize sz) {
    int bits = szbits(sz); uint32_t m = szmask(sz), sb = (uint32_t)(1u << (bits - 1));
    uint32_t fa = a & m, fb = b & m, fr = r & m;
    g_cpu.SR &= ~0x1Fu;
    if (!fr)                                   g_cpu.SR |= SR_Z;
    if (fr >> (bits - 1))                      g_cpu.SR |= SR_N;
    if (fb > fa)                             { g_cpu.SR |= SR_C; g_cpu.SR |= SR_X; }
    if (((fa ^ fb) & sb) && ((fa ^ fr) & sb))  g_cpu.SR |= SR_V;
}
static void flags_cmp(uint32_t a, uint32_t b, uint32_t r, M68KSize sz) {
    int bits = szbits(sz); uint32_t m = szmask(sz), sb = (uint32_t)(1u << (bits - 1));
    uint32_t fa = a & m, fb = b & m, fr = r & m;
    g_cpu.SR &= ~0x0Fu;
    if (!fr)                                   g_cpu.SR |= SR_Z;
    if (fr >> (bits - 1))                      g_cpu.SR |= SR_N;
    if (fb > fa)                               g_cpu.SR |= SR_C;
    if (((fa ^ fb) & sb) && ((fa ^ fr) & sb))  g_cpu.SR |= SR_V;
}

/* Condition-code evaluator — mirrors bcc_cond_expr. */
static int eval_cond(int cc) {
    uint16_t s = g_cpu.SR;
    int C = (s >> 0) & 1, V = (s >> 1) & 1, Z = (s >> 2) & 1, N = (s >> 3) & 1;
    switch (cc & 0xF) {
    case 0x0: return 1;                 /* T  */
    case 0x1: return 0;                 /* F  */
    case 0x2: return !C && !Z;          /* HI */
    case 0x3: return C || Z;            /* LS */
    case 0x4: return !C;                /* CC/HS */
    case 0x5: return C;                 /* CS/LO */
    case 0x6: return !Z;                /* NE */
    case 0x7: return Z;                 /* EQ */
    case 0x8: return !V;                /* VC */
    case 0x9: return V;                 /* VS */
    case 0xA: return !N;                /* PL */
    case 0xB: return N;                 /* MI */
    case 0xC: return N == V;            /* GE */
    case 0xD: return N != V;            /* LT */
    case 0xE: return !Z && (N == V);    /* GT */
    case 0xF: return Z || (N != V);     /* LE */
    default:  return 0;
    }
}

/* ---- stack helpers (active SP = A[7]) ---- */
static void push32(uint32_t v) { g_cpu.A[7] -= 4; m68k_write32(g_cpu.A[7], v); }
static uint32_t pop32(void)    { uint32_t v = m68k_read32(g_cpu.A[7]); g_cpu.A[7] += 4; return v; }

/* sign-extend a size-width value to 32 bits */
static uint32_t sext(uint32_t v, M68KSize sz) {
    switch (sz) { case M68K_SIZE_B: return (uint32_t)(int32_t)(int8_t)v;
                  case M68K_SIZE_W: return (uint32_t)(int32_t)(int16_t)v;
                  default:          return v; }
}
static void push16(uint16_t v) { g_cpu.A[7] -= 2; m68k_write16(g_cpu.A[7], v); }
static uint16_t pop16(void)     { uint16_t v = m68k_read16(g_cpu.A[7]); g_cpu.A[7] += 2; return v; }

/* =========================================================================
 * Shift/rotate engine — direct port of code_generator.c's MN_LSL..MN_ROXR
 * (register Dn target). Computes the result + C/V, then applies the SR tail
 * (emit_shift_sr_update / the rotate tails). Returns 0 on success, -1 if it
 * declines (ROXL/ROXR .L: the generated C's `X << 32` is undefined — halt
 * loudly rather than guess). reg_count: count from D[creg]; else imm_count.
 * ========================================================================= */
static int do_shift(M68KMnemonic mn, int dreg, M68KSize sz,
                    int reg_count, int creg, int imm_count) {
    int bits = szbits(sz);
    uint32_t m = szmask(sz);
    int cnt = reg_count ? (int)(g_cpu.D[creg] & 63u) : imm_count;
    uint32_t sv = g_cpu.D[dreg] & m;
    uint32_t sbit = (uint32_t)(1u << (bits - 1));
    uint32_t res = sv, c = 0, v = 0;
    int with_v = 0, touches_x = 1;

    switch (mn) {
    case MN_LSL:
        res = (cnt > 0 && cnt < bits) ? ((sv << cnt) & m) : (cnt == 0 ? sv : 0u);
        c   = (cnt > 0 && cnt <= bits) ? ((sv >> (bits - cnt)) & 1u) : 0u;
        break;
    case MN_LSR:
        res = (cnt > 0 && cnt < bits) ? (sv >> cnt) : (cnt == 0 ? sv : 0u);
        c   = (cnt > 0 && cnt <= bits) ? ((sv >> (cnt - 1)) & 1u) : 0u;
        break;
    case MN_ASL:
        res = (cnt > 0 && cnt < bits) ? ((sv << cnt) & m) : (cnt == 0 ? sv : 0u);
        c   = (cnt > 0 && cnt <= bits) ? ((sv >> (bits - cnt)) & 1u) : 0u;
        with_v = 1;
        /* V = sign bit changed at ANY point during the shift (HW-accurate).
         * Equivalent: the top (cnt+1) bits of the source are neither all-0 nor
         * all-1. (The generated C approximates this as initial-vs-final MSB,
         * which differs when the sign oscillates an even number of times —
         * a latent recompiler imperfection; the floor matches real hardware.) */
        if (cnt > 0) {
            if (cnt >= bits) v = (sv != 0) ? 1u : 0u;
            else {
                uint32_t top = sv >> (bits - 1 - cnt);
                uint32_t allm = (uint32_t)((1u << (cnt + 1)) - 1u);
                v = (top != 0 && top != allm) ? 1u : 0u;
            }
        }
        break;
    case MN_ASR: {
        int32_t ssv = (int32_t)sext(sv, sz);
        if (cnt > 0 && cnt < bits)      res = (uint32_t)(ssv >> cnt) & m;
        else if (cnt == 0)              res = sv;
        else                            res = (ssv < 0) ? m : 0u;
        /* C = last bit shifted out: for cnt >= bits that is the SIGN bit (the
         * value has gone all-sign), not 0 — the generated C approximates this
         * as 0; the floor matches hardware. */
        if (cnt == 0)        c = 0u;
        else if (cnt < bits) c = (sv >> (cnt - 1)) & 1u;
        else                 c = (sv >> (bits - 1)) & 1u;
        with_v = 1;                                            /* ASR never sets V */
        break;
    }
    case MN_ROL: { touches_x = 0; int cc = cnt % bits;
        res = cc ? (((sv << cc) | (sv >> (bits - cc))) & m) : sv;
        c = cnt ? (res & 1u) : 0u; break; }
    case MN_ROR: { touches_x = 0; int cc = cnt % bits;
        res = cc ? (((sv >> cc) | (sv << (bits - cc))) & m) : sv;
        c = cnt ? ((res >> (bits - 1)) & 1u) : 0u; break; }
    case MN_ROXL: case MN_ROXR: {
        /* rotate through X over a (bits+1)-wide field — use 64-bit so .L works */
        uint64_t x = (g_cpu.SR >> 4) & 1u;
        int period = bits + 1, cc = cnt % period;
        uint64_t wide = (uint64_t)sv | (x << bits);
        uint64_t widemask = (period >= 64) ? ~0ull : ((1ull << period) - 1ull);
        uint64_t rot = (mn == MN_ROXL)
            ? ((wide << cc) | (cc ? (wide >> (period - cc)) : 0)) & widemask
            : ((wide >> cc) | (cc ? (wide << (period - cc)) : 0)) & widemask;
        res = (uint32_t)(rot & m);
        c = (uint32_t)((rot >> bits) & 1u);
        break;
    }
    default: return -1;
    }

    store_dn(dreg, res, sz);

    if (!touches_x) {                                          /* ROL/ROR: preserve X */
        g_cpu.SR &= ~0x0Fu;
        if (!res) g_cpu.SR |= SR_Z;
        if ((res >> (bits - 1)) & 1u) g_cpu.SR |= SR_N;
        if (c) g_cpu.SR |= SR_C;
    } else if (reg_count && cnt == 0) {                        /* count 0: preserve X, C=0 */
        g_cpu.SR &= ~0x0Fu;
        if (!res) g_cpu.SR |= SR_Z;
        if ((res >> (bits - 1)) & 1u) g_cpu.SR |= SR_N;
    } else {
        g_cpu.SR &= ~0x1Fu;
        if (!res) g_cpu.SR |= SR_Z;
        if ((res >> (bits - 1)) & 1u) g_cpu.SR |= SR_N;
        if (c) { g_cpu.SR |= SR_C; g_cpu.SR |= SR_X; }
        if (with_v && v) g_cpu.SR |= SR_V;
    }
    return 0;
}

/* DIVU/DIVS — exact port of clown68000's Action_DIVCommon (the HW-accurate
 * overflow path the generated C omits: on quotient overflow, set V+N, clear Z,
 * and leave Dn UNCHANGED). On real (non-overflow) inputs this matches the
 * generated C; the fuzzer only exercises the overflow edge. */
static void do_div(int dr, uint32_t src16, int is_signed) {
    uint32_t dest = g_cpu.D[dr];
    g_cpu.SR &= ~SR_C;                                   /* carry always cleared */
    if ((uint16_t)src16 == 0) {                          /* div by zero: clown traps (vec 5) */
        g_cpu.SR &= ~(SR_N | SR_Z | SR_V);               /* (harness skips the trap excursion) */
        return;
    }
    int src_neg  = is_signed && ((src16 & 0x8000u) != 0);
    int dest_neg = is_signed && ((dest & 0x80000000u) != 0);
    int res_neg  = (src_neg != dest_neg);
    uint32_t abs_src  = src_neg  ? (uint32_t)(0u - (uint32_t)(int32_t)(int16_t)(uint16_t)src16)
                                 : (uint32_t)(uint16_t)src16;
    uint32_t abs_dest = dest_neg ? (0u - dest) : dest;

    if (abs_src >= (abs_dest >> 16)) {                   /* unsigned overflow pre-check */
        uint32_t abs_quo = abs_dest / abs_src;
        if (!is_signed || abs_quo <= (res_neg ? 0x8000u : 0x7FFFu)) {
            uint32_t abs_rem = abs_dest % abs_src;
            uint32_t quo = res_neg  ? (0u - abs_quo) : abs_quo;
            uint32_t rem = dest_neg ? (0u - abs_rem) : abs_rem;
            g_cpu.D[dr] = (quo & 0xFFFFu) | ((rem & 0xFFFFu) << 16);
            g_cpu.SR &= ~(SR_N | SR_Z | SR_V);
            if (quo & 0x8000u) g_cpu.SR |= SR_N;
            if (quo == 0)      g_cpu.SR |= SR_Z;
            return;
        }
    }
    /* overflow */
    g_cpu.SR |= SR_V; g_cpu.SR |= SR_N; g_cpu.SR &= ~SR_Z;   /* Dn left unchanged */
}

/* ADDX/SUBX — port of code_generator.c (Dn,Dn and -(Ay),-(Ax) forms).
 * X added/subtracted; Z is sticky (cleared only when result!=0). */
static void do_addx_subx(const M68KInstr *ins, M68KSize sz, int is_add) {
    uint16_t w0 = ins->words[0];
    int dst = (w0 >> 9) & 7, src = w0 & 7;
    int bits = szbits(sz), sb = szbytes(sz);
    uint32_t m = szmask(sz), sign = (uint32_t)(1u << (bits - 1));
    uint32_t a, b;
    if (ins->predec_mem_form) {
        int ay_dec = (src == 7 && sz == M68K_SIZE_B) ? 2 : sb;
        int ax_dec = (dst == 7 && sz == M68K_SIZE_B) ? 2 : sb;
        g_cpu.A[src] -= ay_dec; b = mem_read(sz, g_cpu.A[src]) & m;
        g_cpu.A[dst] -= ax_dec; a = mem_read(sz, g_cpu.A[dst]) & m;
    } else { a = g_cpu.D[dst] & m; b = g_cpu.D[src] & m; }
    uint32_t x = (g_cpu.SR >> 4) & 1u;
    uint64_t full = is_add ? ((uint64_t)a + b + x) : ((uint64_t)a - b - x);
    uint32_t r = (uint32_t)full & m;
    int zold = (g_cpu.SR >> 2) & 1;
    g_cpu.SR &= ~0x1Fu;
    if (!r && zold) g_cpu.SR |= SR_Z;
    if (r >> (bits - 1)) g_cpu.SR |= SR_N;
    int carry = is_add ? ((full >> bits) & 1u) : ((full >> 63) & 1u);
    if (carry) { g_cpu.SR |= SR_C; g_cpu.SR |= SR_X; }
    int v = is_add ? (!((a ^ b) & sign) && ((a ^ r) & sign))
                   : ( ((a ^ b) & sign) && ((a ^ r) & sign));
    if (v) g_cpu.SR |= SR_V;
    if (ins->predec_mem_form) mem_write(sz, g_cpu.A[dst], r);
    else                      store_dn(dst, r, sz);
}

/* ABCD/SBCD/NBCD — packed-BCD, port of code_generator.c. op: 0=ABCD 1=SBCD
 * 2=NBCD. Z is sticky; X=C. Operates byte-wide. */
static void do_bcd(const M68KInstr *ins, int op) {
    int x = (g_cpu.SR >> 4) & 1;
    uint8_t a, b = 0, r; int carry; uint32_t a_addr = 0; int to_mem = 0;
    if (op == 2) {                               /* NBCD <ea> (RMW) */
        ExtR er; er_init(&er, ins);
        if (((ins->src_ea >> 3) & 7) <= 1) {     /* Dn/An direct handled via read/store */
            a = (uint8_t)(read_ea_ex(ins, ins->src_ea, M68K_SIZE_B, &er, 1) & 0xFFu);
        } else { a = (uint8_t)(read_ea_ex(ins, ins->src_ea, M68K_SIZE_B, &er, 1) & 0xFFu); }
        int lo = -(int)(a & 0xF) - x;
        int adj_lo = (lo < 0) ? 6 : 0;
        int hi = -(int)(a >> 4) - (lo < 0 ? 1 : 0);
        int adj_hi = (hi < 0) ? 6 : 0;
        int full = (hi & 0xFF) * 16 - adj_hi * 16 + ((lo - adj_lo) & 0xF);
        r = (uint8_t)full; carry = (hi < 0);
        ExtR er2; er_init(&er2, ins);
        write_ea_ex(ins, ins->src_ea, M68K_SIZE_B, &er2, r, 1);
        goto flags;
    }
    int dst = ins->reg, src = ins->src_ea & 7;
    if (ins->predec_mem_form) {
        int ay_dec = (src == 7) ? 2 : 1, ax_dec = (dst == 7) ? 2 : 1;
        g_cpu.A[src] -= ay_dec; b = (uint8_t)m68k_read8(g_cpu.A[src]);
        g_cpu.A[dst] -= ax_dec; a = (uint8_t)m68k_read8(g_cpu.A[dst]);
        a_addr = g_cpu.A[dst]; to_mem = 1;
    } else { a = (uint8_t)g_cpu.D[dst]; b = (uint8_t)g_cpu.D[src]; }
    if (op == 0) {                               /* ABCD */
        unsigned lo = (a & 0xF) + (b & 0xF) + x;
        unsigned adj_lo = (lo > 9) ? 6 : 0;
        unsigned hi = (a >> 4) + (b >> 4) + ((lo + adj_lo) >> 4);
        unsigned adj_hi = (hi > 9) ? 6 : 0;
        unsigned full = (hi << 4) + adj_hi * 16 + ((lo + adj_lo) & 0xF);
        r = (uint8_t)full; carry = (full & 0x100) != 0;
    } else {                                     /* SBCD */
        int lo = (int)(a & 0xF) - (int)(b & 0xF) - x;
        int adj_lo = (lo < 0) ? 6 : 0;
        int hi = (int)(a >> 4) - (int)(b >> 4) - (lo < 0 ? 1 : 0);
        int adj_hi = (hi < 0) ? 6 : 0;
        int full = (hi & 0xFF) * 16 - adj_hi * 16 + ((lo - adj_lo) & 0xF);
        r = (uint8_t)full; carry = (hi < 0);
    }
    if (to_mem) m68k_write8(a_addr, r);
    else        g_cpu.D[dst] = (g_cpu.D[dst] & 0xFFFFFF00u) | r;
flags: {
        int zold = (g_cpu.SR >> 2) & 1;
        g_cpu.SR &= ~SR_V;
        if (r) g_cpu.SR &= ~SR_Z; else if (zold) g_cpu.SR |= SR_Z;
        g_cpu.SR &= ~(SR_C | SR_X | SR_N);
        if (r & 0x80u) g_cpu.SR |= SR_N;
        if (carry) { g_cpu.SR |= SR_C; g_cpu.SR |= SR_X; }
    }
}

/* MOVEM — port of code_generator.c MN_MOVEM. base address resolved from a
 * fresh ExtReader at words[2]; predecrement uses the reversed register mask;
 * mem->reg sign-extends .W to 32 for both An and Dn. */
static void do_movem(const M68KInstr *ins, M68KSize sz) {
    int dir = (ins->words[0] >> 10) & 1;       /* 0 = reg->mem, 1 = mem->reg */
    uint16_t mask = ins->words[1];
    int ea = ins->src_ea, mode = (ea >> 3) & 7, reg = ea & 7;
    int sb = (sz == M68K_SIZE_L) ? 4 : 2;

    ExtR er2; er_init_at(&er2, ins, 2);
    uint32_t base = 0;
    switch (mode) {
    case 2: case 3: case 4: base = g_cpu.A[reg]; break;
    case 5: { int16_t d16 = (int16_t)er_next(&er2); base = (uint32_t)(g_cpu.A[reg] + (int32_t)d16); break; }
    case 6: { uint16_t ext = er_next(&er2); int xreg=(ext>>12)&7,xtype=(ext>>15)&1; int8_t d8=(int8_t)(ext&0xFF);
              int32_t xv=(int32_t)(int16_t)(xtype?g_cpu.A[xreg]:g_cpu.D[xreg]);
              base = (uint32_t)(g_cpu.A[reg] + xv + (int32_t)d8); break; }
    case 7:
        switch (reg) {
        case 0: { int16_t ext=(int16_t)er_next(&er2); base=(uint32_t)(int32_t)ext; break; }
        case 1: base = er_next_dword(&er2); break;
        case 2: { uint32_t pc = ins->addr + er2.bp; int16_t d16=(int16_t)er_next(&er2);
                  base = (uint32_t)((int32_t)pc + d16); break; }
        case 3: { uint32_t pc = ins->addr + er2.bp; uint16_t ext = er_next(&er2);   /* (d8,PC,Xn) */
                  int xreg=(ext>>12)&7,xtype=(ext>>15)&1; int8_t d8=(int8_t)(ext&0xFF);
                  int32_t xv=(int32_t)(int16_t)(xtype?g_cpu.A[xreg]:g_cpu.D[xreg]);
                  base = (uint32_t)(pc + xv + (int32_t)d8); break; }
        default: break;
        }
        break;
    default: break;
    }

    if (dir == 0 && mode == 4) {                /* reg->mem, predecrement: reversed mask */
        uint32_t mb = base;
        for (int bit = 0; bit < 16; bit++) {
            if (!(mask & (1u << bit))) continue;
            int is_an = (bit < 8); int ridx = is_an ? (7 - bit) : (15 - bit);
            uint32_t rv = is_an ? g_cpu.A[ridx] : g_cpu.D[ridx];
            mb -= sb;
            if (sz == M68K_SIZE_L) m68k_write32(mb, rv); else m68k_write16(mb, (uint16_t)rv);
        }
        g_cpu.A[reg] = mb;
    } else if (dir == 0) {                       /* reg->mem, standard */
        uint32_t mb = base;
        for (int bit = 0; bit < 16; bit++) {
            if (!(mask & (1u << bit))) continue;
            int is_an = (bit >= 8); int ridx = is_an ? (bit - 8) : bit;
            uint32_t rv = is_an ? g_cpu.A[ridx] : g_cpu.D[ridx];
            if (sz == M68K_SIZE_L) { m68k_write32(mb, rv); mb += 4; }
            else                   { m68k_write16(mb, (uint16_t)rv); mb += 2; }
        }
        if (mode == 3) g_cpu.A[reg] = mb;
    } else {                                     /* mem->reg, standard */
        uint32_t mb = base;
        for (int bit = 0; bit < 16; bit++) {
            if (!(mask & (1u << bit))) continue;
            int is_an = (bit >= 8); int ridx = is_an ? (bit - 8) : bit;
            if (sz == M68K_SIZE_L) {
                uint32_t v = m68k_read32(mb); mb += 4;
                if (is_an) g_cpu.A[ridx] = v; else g_cpu.D[ridx] = v;
            } else {
                uint32_t v = (uint32_t)(int32_t)(int16_t)m68k_read16(mb); mb += 2;  /* .W sign-extends to 32 */
                if (is_an) g_cpu.A[ridx] = v; else g_cpu.D[ridx] = v;
            }
        }
        if (mode == 3) g_cpu.A[reg] = mb;
    }
}

/* =========================================================================
 * Single-instruction execution.
 *
 * Returns M68KI_OK and writes *next_pc on success; returns M68KI_HALT_UNIMPL
 * (with bad_pc/op recorded) for anything not yet implemented.
 * ========================================================================= */
static M68kiStatus exec_one(const M68KInstr *ins, uint32_t *next_pc) {
    ExtR er; er_init(&er, ins);
    uint32_t fall = ins->addr + ins->byte_length;
    *next_pc = fall;
    M68KSize sz = ins->size;
    int dir = (ins->words[0] >> 8) & 1;
    int cc  = (ins->words[0] >> 8) & 0xF;

    switch (ins->mnemonic) {
    case MN_NOP:
        return M68KI_OK;

    case MN_MOVE: {
        /* MOVE to An (illegal — would be MOVEA) is caught generically by the
         * write_ea An guard + the step wrapper's s_illegal_ea check. */
        uint32_t v = read_ea(ins, ins->src_ea, sz, &er);
        write_ea(ins, ins->dst_ea, sz, &er, v);
        flags_logic(v, sz);
        return M68KI_OK;
    }
    case MN_MOVEA: {
        uint32_t v = read_ea(ins, ins->src_ea, sz, &er);
        g_cpu.A[ins->reg] = sext(v, sz);   /* .W sign-extends to 32; no flags */
        return M68KI_OK;
    }
    case MN_MOVEQ:
        g_cpu.D[ins->reg] = (uint32_t)(int32_t)(int8_t)ins->imm32;
        flags_logic(g_cpu.D[ins->reg], M68K_SIZE_L);
        return M68KI_OK;

    case MN_LEA:
        g_cpu.A[ins->reg] = addr_ea(ins, ins->src_ea, &er);
        return M68KI_OK;
    case MN_PEA: {
        uint32_t a = addr_ea(ins, ins->src_ea, &er);
        push32(a);
        return M68KI_OK;
    }

    case MN_TST: {
        uint32_t v = read_ea(ins, ins->src_ea, sz, &er);
        flags_logic(v, sz);
        return M68KI_OK;
    }
    case MN_CLR: {
        /* 68000 quirk: CLR does a (discarded) read before writing 0. */
        write_ea(ins, ins->src_ea, sz, &er, 0);
        g_cpu.SR = (uint16_t)((g_cpu.SR & ~0x0Fu) | SR_Z);
        return M68KI_OK;
    }

    case MN_EXT:
        if (sz == M68K_SIZE_W) { uint32_t v = (uint32_t)(int32_t)(int8_t)g_cpu.D[ins->reg];
                                 store_dn(ins->reg, v, M68K_SIZE_W); flags_logic(v, M68K_SIZE_W); }
        else                   { uint32_t v = (uint32_t)(int32_t)(int16_t)g_cpu.D[ins->reg];
                                 g_cpu.D[ins->reg] = v;             flags_logic(v, M68K_SIZE_L); }
        return M68KI_OK;
    case MN_SWAP: {
        uint32_t v = g_cpu.D[ins->reg];
        v = (v >> 16) | (v << 16);
        g_cpu.D[ins->reg] = v;
        flags_logic(v, M68K_SIZE_L);
        return M68KI_OK;
    }
    case MN_EXG: {
        /* Exchange two 32-bit registers. Encoding's opmode picks the pairing;
         * the decoder gives the two register indices in reg (Rx) and src_ea&7
         * (Ry) with the bank chosen by opmode bits. We reconstruct from words[0]. */
        uint16_t w = ins->words[0];
        int opmode = (w >> 3) & 0x1F, rx = (w >> 9) & 7, ry = w & 7;
        uint32_t *px, *py;
        if      (opmode == 0x08) { px = &g_cpu.D[rx]; py = &g_cpu.D[ry]; } /* D,D */
        else if (opmode == 0x09) { px = &g_cpu.A[rx]; py = &g_cpu.A[ry]; } /* A,A */
        else                     { px = &g_cpu.D[rx]; py = &g_cpu.A[ry]; } /* D,A (0x11) */
        uint32_t t = *px; *px = *py; *py = t;
        return M68KI_OK;
    }

    /* ---- ADD/SUB (arith, reg<->EA, both directions) ---- */
    case MN_ADD: case MN_SUB: {
        int isadd = (ins->mnemonic == MN_ADD);
        int dreg = ins->reg; uint32_t m = szmask(sz);
        if (dir == 0) {
            uint32_t b = read_ea(ins, ins->src_ea, sz, &er) & m;
            uint32_t a = g_cpu.D[dreg] & m;
            uint32_t r = isadd ? (a + b) : (a - b);
            if (isadd) flags_add(a, b, r, sz); else flags_sub(a, b, r, sz);
            store_dn(dreg, r, sz);
        } else {
            ExtR save = er;
            uint32_t a = read_ea_ex(ins, ins->src_ea, sz, &er, 1) & m;
            uint32_t b = g_cpu.D[dreg] & m;
            uint32_t r = isadd ? (a + b) : (a - b);
            if (isadd) flags_add(a, b, r, sz); else flags_sub(a, b, r, sz);
            er = save;
            write_ea_ex(ins, ins->src_ea, sz, &er, r, 1);
        }
        return M68KI_OK;
    }
    case MN_ADDA: case MN_SUBA: {
        uint32_t v = sext(read_ea(ins, ins->src_ea, sz, &er), sz);
        if (ins->mnemonic == MN_ADDA) g_cpu.A[ins->reg] += v;
        else                          g_cpu.A[ins->reg] -= v;
        return M68KI_OK;   /* address ops set no flags */
    }
    case MN_CMP: {
        uint32_t m = szmask(sz);
        uint32_t b = read_ea(ins, ins->src_ea, sz, &er) & m;
        uint32_t a = g_cpu.D[ins->reg] & m;
        flags_cmp(a, b, a - b, sz);
        return M68KI_OK;
    }
    case MN_CMPA: {
        uint32_t b = sext(read_ea(ins, ins->src_ea, sz, &er), sz);
        uint32_t a = g_cpu.A[ins->reg];
        flags_cmp(a, b, a - b, M68K_SIZE_L);
        return M68KI_OK;
    }
    case MN_CMPM: {
        /* CMPM (Ay)+,(Ax)+ : Ax is dest (ins->reg), Ay is src (src_ea&7). */
        int ay = ins->src_ea & 7, ax = ins->reg, sb = szbytes(sz); uint32_t m = szmask(sz);
        uint32_t b = mem_read(sz, g_cpu.A[ay]) & m; g_cpu.A[ay] += sb;
        uint32_t a = mem_read(sz, g_cpu.A[ax]) & m; g_cpu.A[ax] += sb;
        flags_cmp(a, b, a - b, sz);
        return M68KI_OK;
    }

    /* ---- AND/OR/EOR (logic, reg<->EA) ---- */
    case MN_AND: case MN_OR: {
        int isand = (ins->mnemonic == MN_AND);
        int dreg = ins->reg; uint32_t m = szmask(sz);
        if (dir == 0) {
            uint32_t b = read_ea(ins, ins->src_ea, sz, &er) & m;
            uint32_t a = g_cpu.D[dreg] & m;
            uint32_t r = isand ? (a & b) : (a | b);
            store_dn(dreg, r, sz); flags_logic(r, sz);
        } else {
            ExtR save = er;
            uint32_t a = read_ea_ex(ins, ins->src_ea, sz, &er, 1) & m;
            uint32_t b = g_cpu.D[dreg] & m;
            uint32_t r = isand ? (a & b) : (a | b);
            flags_logic(r, sz);
            er = save;
            write_ea_ex(ins, ins->src_ea, sz, &er, r, 1);
        }
        return M68KI_OK;
    }
    case MN_EOR: {   /* EOR has only the Dn->EA (dir=1 style) form */
        int dreg = ins->reg; uint32_t m = szmask(sz);
        ExtR save = er;
        uint32_t a = read_ea_ex(ins, ins->src_ea, sz, &er, 1) & m;
        uint32_t r = (a ^ (g_cpu.D[dreg] & m));
        flags_logic(r, sz);
        er = save;
        write_ea_ex(ins, ins->src_ea, sz, &er, r, 1);
        return M68KI_OK;
    }

    /* ---- ADDQ/SUBQ ---- */
    case MN_ADDQ: case MN_SUBQ: {
        int isadd = (ins->mnemonic == MN_ADDQ);
        uint32_t q = ins->imm32 ? ins->imm32 : 8u;
        int mode = (ins->src_ea >> 3) & 7;
        if (mode == 1) {                 /* to An: full 32-bit, no flags */
            int r = ins->src_ea & 7;
            g_cpu.A[r] += isadd ? (int32_t)q : -(int32_t)q;
            return M68KI_OK;
        }
        uint32_t m = szmask(sz);
        ExtR save = er;
        uint32_t a = read_ea_ex(ins, ins->src_ea, sz, &er, 1) & m;
        uint32_t r = isadd ? (a + q) : (a - q);
        if (isadd) flags_add(a, q, r, sz); else flags_sub(a, q, r, sz);
        er = save;
        write_ea_ex(ins, ins->src_ea, sz, &er, r, 1);
        return M68KI_OK;
    }

    /* ---- immediate-RMW (ADDI/SUBI/CMPI/ANDI/ORI/EORI) ---- */
    case MN_ADDI: case MN_SUBI: case MN_CMPI:
    case MN_ANDI: case MN_ORI:  case MN_EORI: {
        uint32_t imm = er_next_imm(&er, sz);   /* immediate first in the stream */
        uint32_t m = szmask(sz);
        if (ins->mnemonic == MN_CMPI) {
            /* CMPI is read-only: for (An)+ it MUST post-increment (rmw=0). */
            uint32_t a = read_ea(ins, ins->src_ea, sz, &er) & m;
            flags_cmp(a, imm & m, a - (imm & m), sz);
            return M68KI_OK;
        }
        ExtR after_imm = er;                   /* EA ext words start here */
        uint32_t a = read_ea_ex(ins, ins->src_ea, sz, &er, 1) & m;
        uint32_t r, im = imm & m;
        int arith = 0;
        switch (ins->mnemonic) {
        case MN_ADDI: r = a + im;  arith = 1; break;
        case MN_SUBI: r = a - im;  arith = 2; break;
        case MN_ANDI: r = a & im;            break;
        case MN_ORI:  r = a | im;            break;
        default:      r = a ^ im;            break;  /* EORI */
        }
        if (arith == 1)      flags_add(a, im, r, sz);
        else if (arith == 2) flags_sub(a, im, r, sz);
        else                 flags_logic(r, sz);
        er = after_imm;
        write_ea_ex(ins, ins->src_ea, sz, &er, r, 1);
        return M68KI_OK;
    }

    /* ---- control flow ---- */
    case MN_BRA:
        *next_pc = ins->target_addr;
        return M68KI_OK;
    case MN_Bcc:
        if (eval_cond(cc)) *next_pc = ins->target_addr;
        return M68KI_OK;
    case MN_BSR:
        discover(ins->target_addr);
        push32(fall);
        *next_pc = ins->target_addr;
        return M68KI_OK;
    case MN_DBcc: {
        if (eval_cond(cc)) return M68KI_OK;          /* condition true -> fall through */
        uint32_t dn = (g_cpu.D[ins->reg] & 0xFFFFu);
        dn = (dn - 1) & 0xFFFFu;
        store_dn(ins->reg, dn, M68K_SIZE_W);
        if ((int16_t)dn != -1) *next_pc = ins->target_addr;
        return M68KI_OK;
    }
    case MN_JMP:
        *next_pc = addr_ea(ins, ins->src_ea, &er);
        discover(*next_pc);
        return M68KI_OK;
    case MN_JSR: {
        uint32_t tgt = addr_ea(ins, ins->src_ea, &er);
        discover(tgt);
        push32(fall);
        *next_pc = tgt;
        return M68KI_OK;
    }
    case MN_RTS:
        *next_pc = pop32();
        return M68KI_OK;
    case MN_RTR: {                       /* pop CCR (word), then PC (long) */
        uint16_t ccr = pop16();
        g_cpu.SR = (uint16_t)((g_cpu.SR & 0xFF00u) | (ccr & 0xFFu));
        *next_pc = pop32();
        return M68KI_OK;
    }
    case MN_RTE: {                       /* pop SR, PC, format word (CeDImu RTE) */
        g_cpu.SR = pop16() & 0xA71Fu;     /* mask to valid SR bits (T,S,I,CCR) */
        *next_pc = pop32();
        uint16_t fmt = pop16();           /* format/vector word */
        if ((fmt & 0xF000u) == 0xF000u)   /* long (bus/address-error) frame */
            g_cpu.A[7] += 26;             /* discard the remaining internal frame words */
        return M68KI_OK;
    }
    case MN_TRAP:                        /* TRAP #n → vector 32+n. Stack the NEXT
                                          * instruction's PC (post-TRAP) — for
                                          * OS-9, the inline service-code word —
                                          * then vector to the kernel handler the
                                          * loop continues into (build_exception_
                                          * frame sets g_cpu.PC = handler). */
        g_cpu.PC = (ins->addr + ins->byte_length) & 0xFFFFFFu;
        m68k_raise_exception_frame((uint8_t)(0x20u + (ins->imm32 & 0xFu)));
        *next_pc = g_cpu.PC;
        return M68KI_OK;

    /* ---- shifts / rotates (register Dn target) ---- */
    case MN_LSL: case MN_LSR: case MN_ASL: case MN_ASR:
    case MN_ROL: case MN_ROR: case MN_ROXL: case MN_ROXR: {
        if (ins->mem_shift) {            /* 1-bit shift of a word in memory */
            ExtR save = er;
            uint32_t sv = read_ea_ex(ins, ins->src_ea, M68K_SIZE_W, &er, 1) & 0xFFFFu;
            /* reuse do_shift by faking a Dn: load into a scratch via D-less path */
            uint32_t res, c = 0, v = 0; int with_v = 0, touches_x = 1;
            switch (ins->mnemonic) {
            case MN_LSL: res = (sv << 1) & 0xFFFFu; c = (sv >> 15) & 1u; break;
            case MN_LSR: res = sv >> 1;             c = sv & 1u;        break;
            case MN_ASL: res = (sv << 1) & 0xFFFFu; c = (sv >> 15) & 1u; with_v = 1;
                         if ((sv & 0x8000u) != (res & 0x8000u)) v = 1;  break;
            case MN_ASR: res = (uint32_t)(((int16_t)sv) >> 1) & 0xFFFFu; c = sv & 1u; with_v = 1; break;
            case MN_ROL: touches_x = 0; res = ((sv << 1) | (sv >> 15)) & 0xFFFFu; c = (sv >> 15) & 1u; break;
            case MN_ROR: touches_x = 0; res = ((sv >> 1) | (sv << 15)) & 0xFFFFu; c = sv & 1u; break;
            case MN_ROXL: { uint32_t x=(g_cpu.SR>>4)&1u; res=((sv<<1)|x)&0xFFFFu; c=(sv>>15)&1u; break; }
            default:      { uint32_t x=(g_cpu.SR>>4)&1u; res=((sv>>1)|(x<<15))&0xFFFFu; c=sv&1u; break; } /* ROXR */
            }
            er = save;
            write_ea_ex(ins, ins->src_ea, M68K_SIZE_W, &er, res, 1);
            if (!touches_x) { g_cpu.SR &= ~0x0Fu; if(!res)g_cpu.SR|=SR_Z; if(res&0x8000u)g_cpu.SR|=SR_N; if(c)g_cpu.SR|=SR_C; }
            else { g_cpu.SR &= ~0x1Fu; if(!res)g_cpu.SR|=SR_Z; if(res&0x8000u)g_cpu.SR|=SR_N;
                   if(c){g_cpu.SR|=SR_C;g_cpu.SR|=SR_X;} if(with_v&&v)g_cpu.SR|=SR_V; }
            return M68KI_OK;
        }
        int reg_count = (ins->src_ea >= 0);
        if (do_shift(ins->mnemonic, ins->reg, sz, reg_count, ins->src_ea, (int)(ins->imm32 & 63u)) != 0) {
            g_m68ki_bad_pc = ins->addr; g_m68ki_bad_op = ins->words[0];
            return M68KI_HALT_UNIMPL;
        }
        return M68KI_OK;
    }

    /* ---- bit ops ---- */
    case MN_BTST: case MN_BCHG: case MN_BCLR: case MN_BSET: {
        int is_imm = (ins->reg < 0);
        int ea = ins->src_ea, ea_mode = (ea >> 3) & 7;
        int bit_mask = (ea_mode == 0) ? 31 : 7;
        M68KSize bsz = (ea_mode == 0) ? M68K_SIZE_L : M68K_SIZE_B;
        uint32_t bn;
        if (is_imm) { bn = ins->imm32 & (uint32_t)bit_mask; er_init_at(&er, ins, 2); }
        else        { bn = (uint32_t)g_cpu.D[ins->reg] & (uint32_t)bit_mask; }
        if (ins->mnemonic == MN_BTST) {
            /* BTST is read-only: for (An)+ it MUST post-increment (rmw=0). */
            uint32_t src = read_ea(ins, ea, bsz, &er) & szmask(bsz);
            g_cpu.SR = (uint16_t)((g_cpu.SR & ~SR_Z) | ((!(src & (1u << bn))) ? SR_Z : 0u));
            return M68KI_OK;
        }
        ExtR save = er;                                  /* BCHG/BCLR/BSET: RMW */
        uint32_t src = read_ea_ex(ins, ea, bsz, &er, 1) & szmask(bsz);
        g_cpu.SR = (uint16_t)((g_cpu.SR & ~SR_Z) | ((!(src & (1u << bn))) ? SR_Z : 0u));
        uint32_t res = (ins->mnemonic == MN_BCHG) ? (src ^ (1u << bn))
                     : (ins->mnemonic == MN_BCLR) ? (src & ~(1u << bn))
                                                  : (src | (1u << bn));
        er = save;
        write_ea_ex(ins, ea, bsz, &er, res, 1);
        return M68KI_OK;
    }

    /* ---- NEG / NEGX / NOT ---- */
    case MN_NEG: case MN_NEGX: case MN_NOT: {
        uint32_t m = szmask(sz);
        ExtR save = er;
        uint32_t a = read_ea_ex(ins, ins->src_ea, sz, &er, 1) & m;
        uint32_t r;
        if (ins->mnemonic == MN_NOT) { r = (~a) & m; flags_logic(r, sz); }
        else {
            uint32_t x = (ins->mnemonic == MN_NEGX) ? ((g_cpu.SR >> 4) & 1u) : 0u;
            r = (0u - a - x) & m;
            flags_sub(0u, a, r, sz);
        }
        er = save;
        write_ea_ex(ins, ins->src_ea, sz, &er, r, 1);
        return M68KI_OK;
    }

    /* ---- TAS ---- */
    case MN_TAS: {
        ExtR save = er;
        uint32_t v = read_ea_ex(ins, ins->src_ea, M68K_SIZE_B, &er, 1) & 0xFFu;
        g_cpu.SR &= ~0x0Fu;
        if (!v) g_cpu.SR |= SR_Z;
        if (v >> 7) g_cpu.SR |= SR_N;
        er = save;
        write_ea_ex(ins, ins->src_ea, M68K_SIZE_B, &er, v | 0x80u, 1);
        return M68KI_OK;
    }

    /* ---- MUL / DIV (.W source, .L Dn result) ---- */
    case MN_MULU: {
        uint32_t s = read_ea(ins, ins->src_ea, M68K_SIZE_W, &er) & 0xFFFFu;
        g_cpu.D[ins->reg] = (uint32_t)(uint16_t)g_cpu.D[ins->reg] * s;
        flags_logic(g_cpu.D[ins->reg], M68K_SIZE_L);
        return M68KI_OK;
    }
    case MN_MULS: {
        int32_t s = (int32_t)(int16_t)(read_ea(ins, ins->src_ea, M68K_SIZE_W, &er) & 0xFFFFu);
        g_cpu.D[ins->reg] = (uint32_t)((int32_t)(int16_t)g_cpu.D[ins->reg] * s);
        flags_logic(g_cpu.D[ins->reg], M68K_SIZE_L);
        return M68KI_OK;
    }
    case MN_DIVU:
        do_div(ins->reg, read_ea(ins, ins->src_ea, M68K_SIZE_W, &er) & 0xFFFFu, 0);
        return M68KI_OK;
    case MN_DIVS:
        do_div(ins->reg, read_ea(ins, ins->src_ea, M68K_SIZE_W, &er) & 0xFFFFu, 1);
        return M68KI_OK;

    /* ---- MOVEM ---- */
    case MN_MOVEM:
        do_movem(ins, sz);
        return M68KI_OK;

    /* ---- LINK / UNLK ---- */
    case MN_LINK: {
        int ar = ins->reg; int16_t disp = (int16_t)ins->words[1];
        g_cpu.A[7] -= 4; m68k_write32(g_cpu.A[7], g_cpu.A[ar]);
        g_cpu.A[ar] = g_cpu.A[7];
        g_cpu.A[7] += (int32_t)disp;
        return M68KI_OK;
    }
    case MN_UNLK: {
        int ar = ins->reg;
        g_cpu.A[7] = g_cpu.A[ar];
        g_cpu.A[ar] = m68k_read32(g_cpu.A[7]);
        g_cpu.A[7] += 4;
        return M68KI_OK;
    }

    /* ---- Scc ---- */
    case MN_Scc: {
        uint8_t val = eval_cond(cc) ? 0xFFu : 0x00u;
        write_ea(ins, ins->src_ea, M68K_SIZE_B, &er, val);
        return M68KI_OK;
    }

    /* ---- MOVE to/from SR / CCR / USP ---- */
    case MN_MOVE_SR:
        if (!ins->dst_is_ea) g_cpu.SR = (uint16_t)(read_ea(ins, ins->src_ea, M68K_SIZE_W, &er) & 0xA71Fu);
        else                 write_ea(ins, ins->src_ea, M68K_SIZE_W, &er, g_cpu.SR);
        return M68KI_OK;
    case MN_MOVE_CCR:        /* CCR is 5 bits (0x1F) — clown masks; matches HW */
        if (!ins->dst_is_ea)
            g_cpu.SR = (uint16_t)((g_cpu.SR & 0xFF00u) | (read_ea(ins, ins->src_ea, M68K_SIZE_W, &er) & 0x1Fu));
        else
            write_ea(ins, ins->src_ea, M68K_SIZE_W, &er, (uint16_t)(g_cpu.SR & 0x001Fu));
        return M68KI_OK;
    case MN_MOVE_USP:
        if ((ins->words[0] >> 3) & 1) g_cpu.A[ins->reg] = g_cpu.USP;   /* USP->An */
        else                          g_cpu.USP = g_cpu.A[ins->reg];   /* An->USP */
        return M68KI_OK;

    /* ---- ADDX / SUBX ---- */
    case MN_ADDX: do_addx_subx(ins, sz, 1); return M68KI_OK;
    case MN_SUBX: do_addx_subx(ins, sz, 0); return M68KI_OK;

    /* ---- packed BCD ---- */
    case MN_ABCD: do_bcd(ins, 0); return M68KI_OK;
    case MN_SBCD: do_bcd(ins, 1); return M68KI_OK;
    case MN_NBCD: do_bcd(ins, 2); return M68KI_OK;

    /* ---- immediate to SR / CCR (then mask to valid SR bits, like HW) ---- */
    case MN_ORI_TO_CCR:  g_cpu.SR |= (uint16_t)(ins->imm32 & 0xFFu);              g_cpu.SR &= 0xA71Fu; return M68KI_OK;
    case MN_ORI_TO_SR:   g_cpu.SR |= (uint16_t)(ins->imm32 & 0xFFFFu);            g_cpu.SR &= 0xA71Fu; return M68KI_OK;
    case MN_ANDI_TO_CCR: g_cpu.SR &= (uint16_t)(0xFF00u | (ins->imm32 & 0xFFu));  g_cpu.SR &= 0xA71Fu; return M68KI_OK;
    case MN_ANDI_TO_SR:  g_cpu.SR &= (uint16_t)(ins->imm32 & 0xFFFFu);            g_cpu.SR &= 0xA71Fu; return M68KI_OK;
    case MN_EORI_TO_CCR: g_cpu.SR ^= (uint16_t)(ins->imm32 & 0xFFu);              g_cpu.SR &= 0xA71Fu; return M68KI_OK;
    case MN_EORI_TO_SR:  g_cpu.SR ^= (uint16_t)(ins->imm32 & 0xFFFFu);            g_cpu.SR &= 0xA71Fu; return M68KI_OK;

    default:
        g_m68ki_bad_pc = ins->addr;
        g_m68ki_bad_op = ins->words[0];
        return M68KI_HALT_UNIMPL;
    }
}

/* =========================================================================
 * m68k_cycles — clean-room SCC68070 per-instruction cycle cost (MC-CDI-005).
 *
 * The interpreter must advance device timing (MCD212 DA, timers) at the SAME
 * rate the CeDImu oracle does, or it drifts out of phase on the boot's
 * vertical-sync poll loops (BTST #7,$4FFFF1 / BNE). CeDImu sums a per-handler
 * `calcTime` per instruction; this mirrors that table EXACTLY. The values are
 * SCC68070 timings — NOT a plain 68000 (PRM) and NOT clown68000: the 68070's
 * costs differ (e.g. NOP=7, MOVE reg-reg=7, RTS=15), so matching the *oracle*
 * is what keeps lockstep. Source of truth: external/CeDImu cores/SCC68070/
 * {InstructionSet,AddressingModes,MemoryAccess}.cpp.
 *
 * Must be evaluated in the instruction's ENTRY state (before exec_one mutates
 * g_cpu): a few costs read entry register/flag/stack state (register shift
 * counts, DBcc condition, RTE frame format).
 * ========================================================================= */
static int cyc_popcount16(uint16_t v) {
    int c = 0;
    while (v) { c += v & 1; v >>= 1; }
    return c;
}

/* SCC68070 effective-address calculation time (CeDImu IT* constants,
 * AddressingModes.cpp / MemoryAccess.cpp). sb = operand size in bytes; only
 * byte/word (<4 → "BW") vs long (==4 → "L") matters. Dn/An cost nothing;
 * immediate (mode 7, reg 4) costs ITIBW(4)/ITIL(8). */
static int cyc_ea(int ea6, int sb) {
    int mode = (ea6 >> 3) & 7, reg = ea6 & 7;
    int isL = (sb == 4);
    switch (mode) {
    case 0: case 1: return 0;                 /* Dn / An                */
    case 2:         return isL ? 8  : 4;      /* (An)        ITARI      */
    case 3:         return isL ? 8  : 4;      /* (An)+       ITARIWPo   */
    case 4:         return isL ? 11 : 7;      /* -(An)       ITARIWPr   */
    case 5:         return isL ? 15 : 11;     /* d16(An)     ITARIWD    */
    case 6:         return isL ? 18 : 14;     /* d8(An,Xn)   ITARIWI8   */
    case 7:
        switch (reg) {
        case 0: return isL ? 12 : 8;          /* (xxx).W     ITAS       */
        case 1: return isL ? 16 : 12;         /* (xxx).L     ITAL       */
        case 2: return isL ? 15 : 11;         /* d16(PC)     ITPCIWD    */
        case 3: return isL ? 18 : 14;         /* d8(PC,Xn)   ITPCIWI8   */
        case 4: return isL ? 8  : 4;          /* #imm        ITIL/ITIBW */
        }
    }
    return 0;
}

static int sb_of(M68KSize sz) {
    return sz == M68K_SIZE_L ? 4 : sz == M68K_SIZE_B ? 1 : 2;
}

/* Cycle cost of `ins`, mirroring CeDImu's SCC68070 instruction handlers. */
int m68k_cycles(const M68KInstr *ins) {
    const uint16_t op     = ins->words[0];
    const int      eamode = (op >> 3) & 7;
    const int      eareg  =  op       & 7;
    const int      ea6    =  op & 0x3F;          /* EA field = low 6 bits   */
    const int      sb     = sb_of(ins->size);
    const int      isL    = (sb == 4);

    switch (ins->mnemonic) {

    /* ---- control flow ---- */
    case MN_NOP:    return 7;
    case MN_RTS:    return 15;
    case MN_RTR:    return 22;
    case MN_STOP:   return 13;
    case MN_RESET:  return 154;
    case MN_RTE: {  /* 39 short frame / 146 long frame; format word at SP+6 */
        uint16_t fmt = m68k_read16((g_cpu.A[7] + 6) & 0xFFFFFFu);
        return ((fmt & 0xF000) == 0xF000) ? 146 : 39;
    }
    case MN_BRA:    return 13 + (((op & 0xFF) == 0) ? 1 : 0);  /* +1 for .W disp */
    case MN_Bcc:    return 13 + (((op & 0xFF) == 0) ? 1 : 0);
    case MN_BSR:    return ((op & 0xFF) == 0) ? 22 : 17;
    case MN_DBcc:   return eval_cond((op >> 8) & 0xF) ? 14 : 17;
    case MN_JMP:    return ((eamode == 7 && eareg <= 1) ? 6  : 3)  + cyc_ea(ea6, 1);
    case MN_JSR:    return ((eamode == 7 && eareg <= 1) ? 17 : 14) + cyc_ea(ea6, 1);

    /* ---- moves ---- */
    case MN_MOVE: {  /* base 7 + src-EA + dst-EA (dst = (mode<<3)|reg from 11:6) */
        int dst_ea = (((op >> 6) & 7) << 3) | ((op >> 9) & 7);
        return 7 + cyc_ea(ea6, sb) + cyc_ea(dst_ea, sb);
    }
    case MN_MOVEA:  return 7 + cyc_ea(ea6, sb);
    case MN_MOVEQ:  return 7;
    case MN_LEA:    return ((eamode == 7 && eareg <= 1) ? 6  : 3)  + cyc_ea(ea6, 2);
    case MN_PEA:    return ((eamode == 7 && eareg <= 1) ? 13 : 10) + cyc_ea(ea6, 4);
    case MN_MOVEM: {
        uint16_t list = ins->words[1];
        int szl  = (op >> 6) & 1;                 /* 1 = long transfer */
        int base = (((eamode == 7 && eareg <= 1) || eamode <= 4) ? 19 : 16)
                   + (((op >> 10) & 1) ? 3 : 0)   /* dr: mem->reg +3   */
                   + (szl ? -4 : 0);
        return base + cyc_ea(ea6, szl ? 4 : 2)
               + cyc_popcount16(list) * (szl ? 11 : 7);
    }

    /* ---- ALU binary (Dn <-> EA) ---- */
    case MN_ADD: case MN_SUB:
    case MN_AND: case MN_OR:                       /* +write penalty when EA is dst */
        return 7 + cyc_ea(ea6, sb) + (((op >> 8) & 1) ? (isL ? 8 : 4) : 0);
    case MN_EOR:                                   /* EA is dst whenever it's memory */
        return 7 + cyc_ea(ea6, sb) + (eamode ? (isL ? 8 : 4) : 0);
    case MN_CMP:    return 7 + cyc_ea(ea6, sb);
    case MN_ADDA: case MN_SUBA: case MN_CMPA:
        return 7 + cyc_ea(ea6, ((op >> 8) & 1) ? 4 : 2);
    case MN_ADDQ: case MN_SUBQ:
        if (eamode == 1) return 7;                 /* An: flat 7         */
        return 7 + cyc_ea(ea6, sb) + (eamode ? (isL ? 8 : 4) : 0);

    /* ---- ALU immediate ---- */
    case MN_ADDI: case MN_SUBI:                    /* mem: BW+4/L+12; reg: L+4 */
        return 14 + cyc_ea(ea6, sb) + (eamode ? (isL ? 12 : 4) : (isL ? 4 : 0));
    case MN_CMPI:   return 14 + cyc_ea(ea6, sb) + (isL ? 4 : 0);
    case MN_ANDI: case MN_EORI:                    /* base 0; +(L?8/4); +(mem?18:14) */
        return cyc_ea(ea6, sb) + (isL ? (eamode ? 8 : 4) : 0) + (eamode ? 18 : 14);
    case MN_ORI:                                   /* base 14; +(L?8/4); +(mem?4:0) */
        return 14 + cyc_ea(ea6, sb) + (isL ? (eamode ? 8 : 4) : 0) + (eamode ? 4 : 0);

    /* ---- unary ---- */
    case MN_TST:    return 7 + cyc_ea(ea6, sb);
    case MN_CLR:    return 7 + cyc_ea(ea6, sb);
    case MN_NEG: case MN_NEGX: case MN_NOT:
        return 7 + cyc_ea(ea6, sb) + (eamode ? (isL ? 8 : 4) : 0);
    case MN_EXT:    return 7;
    case MN_SWAP:   return 7;
    case MN_NBCD:   return (eamode ? 14 : 10) + cyc_ea(ea6, 1);
    case MN_TAS:    return 10 + cyc_ea(ea6, 1) + (eamode ? 1 : 0);
    case MN_Scc:    return (eamode ? 17 : 13) + cyc_ea(ea6, 1);

    /* ---- shifts / rotates ---- */
    case MN_LSL: case MN_LSR: case MN_ASL: case MN_ASR:
    case MN_ROL: case MN_ROR: case MN_ROXL: case MN_ROXR:
        if (ins->mem_shift)
            return 14 + cyc_ea(ea6, 2);            /* 1-bit word memory shift */
        else {
            int cnt = (op >> 9) & 7;
            int shift = (op & 0x20) ? (int)(g_cpu.D[cnt] % 64) : (cnt ? cnt : 8);
            return 13 + 3 * shift;
        }

    /* ---- multiply / divide (CeDImu uses fixed costs) ---- */
    case MN_MULS: case MN_MULU: return 76  + cyc_ea(ea6, 2);
    case MN_DIVS:               return 169 + cyc_ea(ea6, 2);
    case MN_DIVU:               return 130 + cyc_ea(ea6, 2);

    /* ---- bit ops (static form +7; memory form reads byte + writes) ---- */
    case MN_BTST:   return 7  + ((op & 0x100) ? 0 : 7) + (eamode ? cyc_ea(ea6, 1)      : 0);
    case MN_BCHG: case MN_BCLR: case MN_BSET:
                    return 10 + ((op & 0x100) ? 0 : 7) + (eamode ? cyc_ea(ea6, 1) + 4  : 0);

    /* ---- BCD / extended ---- */
    case MN_ABCD: case MN_SBCD: return ins->predec_mem_form ? 31 : 10;
    case MN_ADDX: case MN_SUBX: return ins->predec_mem_form ? (isL ? 40 : 28) : 7;

    /* ---- link / misc ---- */
    case MN_LINK:     return 25;
    case MN_UNLK:     return 15;
    case MN_EXG:      return 13;
    case MN_MOVE_USP: return 7;
    case MN_MOVEP:    return ((op >> 7) & 1) ? (((op >> 6) & 1) ? 39 : 25)   /* reg->mem */
                                             : (((op >> 6) & 1) ? 36 : 22);  /* mem->reg */
    case MN_CHK:      return 19 + cyc_ea(ea6, 2);
    case MN_MOVE_CCR:                                 /* MOVE <ea>,CCR (or CCR,<ea>) */
    case MN_MOVE_SR:                                  /* MOVE <ea>,SR  (or SR,<ea>)  */
        return ins->dst_is_ea ? (eamode ? 11 : 7) + cyc_ea(ea6, 2)
                              : 10 + cyc_ea(ea6, 2);
    case MN_MOVEC:    return 12;                       /* SCC68070 control-reg move   */

    case MN_ORI_TO_CCR:  case MN_ORI_TO_SR:
    case MN_ANDI_TO_CCR: case MN_ANDI_TO_SR:
    case MN_EORI_TO_CCR: case MN_EORI_TO_SR: return 14;

    case MN_CMPM:     return isL ? 26 : 18;

    /* ---- traps / illegal (exception-processing clock periods) ---- */
    case MN_TRAP:     return 52;   /* TRAP #n → vectors 32..47 */
    case MN_TRAPV:    return 10;   /* untaken; taken path traps via the exception model */
    case MN_ILLEGAL:  return 55;

    case MN_OTHER:
    default:          return 14;   /* unclassified: a reasonable SCC68070 default */
    }
}

/* =========================================================================
 * Public entries.
 * ========================================================================= */

/* Execute one instruction at g_cpu.PC; advance g_cpu.PC. */
M68kiStatus m68k_interp_step(void) {
    uint32_t pc = g_cpu.PC & 0xFFFFFFu;
    if (pc & 1) { g_m68ki_bad_pc = pc; return M68KI_HALT_BADADDR; }  /* odd PC = address error */

    /* Feed the SAME always-on trace ring the recompiled path uses (entry-state
     * sample), so interpreted instructions are first-divergence-comparable with
     * the oracle exactly like recompiled ones — the trace is one continuous
     * per-instruction stream regardless of which tier executed it. */
    debug_trace_block();
    /* Classify this interpreted instruction (PC + why-we-fell-through + region)
     * into the always-on fallback aggregate, so "the interpreter ran X%" becomes
     * "which PCs, for which reason" — see debug_server.h. */
    debug_trace_interp(pc);

    M68KInstr ins;
    if (!m68k_decode(busview(pc), pc, &ins)) {   /* fetch from the CD-i bus (RAM or ROM) */
        g_m68ki_bad_pc = pc; g_m68ki_bad_op = m68k_read16(pc);
        return M68KI_HALT_UNIMPL;
    }
    /* Cost the instruction at ENTRY state — a few costs (register shift counts,
     * DBcc condition, RTE frame format) read registers/flags/stack that exec_one
     * is about to mutate. See m68k_cycles (MC-CDI-005). */
    int cyc = m68k_cycles(&ins);

    /* Publish this instruction's exception context, in case a memory access
     * inside exec_one hits an unmapped address (bus error). CeDImu stacks the
     * post-fetch PC and currentOpcode; byte_length is the post-fetch advance.
     * (For an instruction that source-reads before fetching trailing dest
     * extension words this slightly overshoots — the boot's probe is a register-
     * indirect read with no extension words, so it is exact.) */
    s_fault_pc     = (ins.addr + ins.byte_length) & 0xFFFFFFu;
    g_fault_opcode = ins.words[0];

    /* Arm the mid-instruction bus-error unwind around exec_one. Save/restore the
     * landing pad so a nested interpreter run (the handler this may vector into)
     * leaves the outer frame's pad intact. */
    jmp_buf prev_env;  int prev_armed = s_bus_armed;
    memcpy(prev_env, s_bus_env, sizeof prev_env);
    if (setjmp(s_bus_env) != 0) {
        /* A bus error aborted exec_one. Restore the outer pad, then raise a
         * faithful SCC68070 bus error (vector 2): stack PC = post-fetch PC,
         * TPF = the unmapped address, and continue the loop into the handler. */
        memcpy(s_bus_env, prev_env, sizeof prev_env);
        s_bus_armed = prev_armed;
        g_cpu.PC = s_fault_pc;
        m68k_raise_exception_frame(2);
        mcd212_tick(158);   /* CeDImu ProcessException: BusError = 158 clock periods */
        return M68KI_OK;
    }
    s_bus_armed = 1;

    uint32_t next;
    s_illegal_ea = 0;
    M68kiStatus st = exec_one(&ins, &next);
    memcpy(s_bus_env, prev_env, sizeof prev_env);   /* disarm: restore outer pad */
    s_bus_armed = prev_armed;
    if (st != M68KI_OK) return st;
    if (s_illegal_ea) {                       /* illegal An-destination encoding */
        g_m68ki_bad_pc = ins.addr; g_m68ki_bad_op = ins.words[0];
        return M68KI_HALT_UNIMPL;
    }
    g_cpu.PC = next & 0xFFFFFFu;
    /* Advance MCD212 display timing with this instruction's real SCC68070 cycle
     * cost, so DA toggles in phase with the oracle while the boot polls it from
     * interpreted code. The recompiled tier drains its own real cycles. */
    mcd212_tick(cyc);
    return M68KI_OK;
}

/* Run until PC == stop_pc (or halt). */
M68kiStatus m68k_interp_run(uint32_t entry_pc, uint32_t stop_pc) {
    g_cpu.PC = entry_pc & 0xFFFFFFu;
    g_m68ki_insn_count = 0;
    g_m68ki_discover_count = 0;

    while (g_cpu.PC != (stop_pc & 0xFFFFFFu)) {
        M68kiStatus st = m68k_interp_step();
        if (st != M68KI_OK) return st;
        if (++g_m68ki_insn_count > M68KI_INSN_GUARD) {
            g_m68ki_bad_pc = g_cpu.PC; return M68KI_HALT_GUARD;
        }
    }
    return M68KI_OK;
}

/* Hybrid handoff: interpret until PC re-enters statically recompiled code (a
 * dispatch-table entry) so native execution can resume, or PC == stop_pc, or a
 * halt. See the header. dispatch_has_addr() is provided by the runtime. */
extern int dispatch_has_addr(uint32_t addr);

M68kiStatus m68k_interp_run_until_known(uint32_t entry_pc, uint32_t stop_pc) {
    g_cpu.PC = entry_pc & 0xFFFFFFu;
    g_m68ki_insn_count = 0;
    g_m68ki_discover_count = 0;
    stop_pc &= 0xFFFFFFu;

    for (;;) {
        uint32_t pc = g_cpu.PC & 0xFFFFFFu;
        if (stop_pc && pc == stop_pc) return M68KI_OK;
        /* Re-entered recompiled territory (but not at the entry we started on,
         * so we always make progress): hand back to native at this PC. */
        if (pc != (entry_pc & 0xFFFFFFu) && dispatch_has_addr(pc))
            return (M68kiStatus)M68KI_REENTER;
        M68kiStatus st = m68k_interp_step();
        if (st != M68KI_OK) return st;
        if (++g_m68ki_insn_count > M68KI_INSN_GUARD) {
            g_m68ki_bad_pc = g_cpu.PC; return M68KI_HALT_GUARD;
        }
    }
}
