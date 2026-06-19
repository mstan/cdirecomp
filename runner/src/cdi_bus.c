/*
 * cdi_bus.c — SCC68070 memory bus: RAM banks, system ROM, MMIO routing.
 *
 * Generated code reaches memory ONLY through the m68k_read and m68k_write
 * accessors (verified: the code generator never indexes RAM/ROM arrays
 * directly). That makes this
 * file the single, faithful boundary for the CD-i memory model. Unmapped or
 * not-yet-modelled accesses FAIL LOUD (abort with the exact address + PC) —
 * never a silent default, per the project's no-stub discipline.
 *
 * The SCC68070 has an on-chip MMU that CD-RTOS programs; address translation
 * is not modelled yet (TODO MC-CDI-006). These are the default physical decodes.
 */
#include "cdi_runtime.h"
#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Backing storage: 1 MB system RAM (two 512 KB banks) + 1 MB system ROM window. */
uint8_t g_ram0[CDI_RAM0_SIZE];
uint8_t g_ram1[CDI_RAM1_SIZE];
uint8_t g_rom [CDI_ROM_SIZE];

/* Last operand effective address accessed (CeDImu SCC68070::lastAddress). The
 * bus/address-error frame stacks this as TPF — for an ADDRESS error it is NOT
 * the faulting (odd) target but the last DATA EA touched before it (CeDImu's
 * GetWord/SetWord throw AddressError without updating lastAddress), which the
 * static recompiler cannot know. Set on every operand access; long accesses
 * collapse to the base EA to match CeDImu's once-per-operand GetEffectiveAddress. */
uint32_t g_last_access_addr = 0;

/* Load the CD-RTOS system ROM image into the ROM window ($400000..). The
 * recompiled code reads PC-relative constants/tables straight out of ROM via
 * m68k_read*, so the bytes must be present before execution starts. */
void cdi_bus_load_rom(const uint8_t *src, uint32_t n) {
    if (n > CDI_ROM_SIZE) n = CDI_ROM_SIZE;
    memcpy(g_rom, src, n);
}

static void bus_fault(const char *op, uint32_t addr, int bits) {
    /* A real SCC68070 bus error. When the interpreter is mid-instruction it has
     * armed an unwind: raise the faithful vector-2 exception (CeDImu does the
     * same — see Mono3::GetWord throwing BusError out to ProcessException). This
     * call does not return when armed; control longjmps back into the step,
     * stacks the long frame, and vectors to the OS-9 handler. Only a fault from
     * un-unwindable (recompiled) code reaches the fail-loud path below — that is
     * a genuinely unmodelled region, not an architectural bus error. */
    if (m68k_interp_bus_error(addr)) return;   /* unreachable: longjmp on success */

    fprintf(stderr, "[bus] UNMAPPED %s%d @ $%08X (PC=$%08X) — region not modelled "
                    "(see TODO.md MC-CDI-004)\n", op, bits, addr, g_cpu.PC);
    /* Fail loud WITH exact CPU state: which register holds the bad address tells
     * a memory-sizing probe (computed/boundary address) apart from a divergence
     * (a null/garbage pointer dereferenced), per DEBUG.md first-divergence. */
    for (int i = 0; i < 8; i++)
        fprintf(stderr, "      D%d=$%08X  A%d=$%08X\n", i, g_cpu.D[i], i, g_cpu.A[i]);
    fprintf(stderr, "      SR=$%04X  USP=$%08X  irq_pending=$%08X\n",
            g_cpu.SR, g_cpu.USP, g_irq_pending);
    /* The executed path INTO the fault — turns a final-register dump into a
     * trail (DEBUG.md first-divergence). Always-on ring; no arming needed. */
    debug_dump_fault_trail("unmapped bus access");
    if (g_hold_on_fault) cdi_fault_hold();   /* keep rings queryable for diffing */
    abort();
}

/* Returns a writable pointer into a RAM bank, or NULL if `a` isn't RAM. */
static inline uint8_t *ram_ptr(uint32_t a) {
    if (a - CDI_RAM0_BASE < CDI_RAM0_SIZE) return &g_ram0[a - CDI_RAM0_BASE];
    if (a - CDI_RAM1_BASE < CDI_RAM1_SIZE) return &g_ram1[a - CDI_RAM1_BASE];
    return NULL;
}

/* Region classifier for MMIO. Order matters: MCD212 sits at the top of the
 * ROM window, so test it before the ROM range. */
typedef enum { RGN_RAM, RGN_ROM, RGN_MCD212, RGN_CDIC, RGN_SLAVE, RGN_NVRAM,
               RGN_PERIPH, RGN_NONE } BusRegion;

static BusRegion classify(uint32_t a) {
    if (a - CDI_RAM0_BASE < CDI_RAM0_SIZE)              return RGN_RAM;
    if (a - CDI_RAM1_BASE < CDI_RAM1_SIZE)              return RGN_RAM;
    if (a >= CDI_MCD212_BASE && a <= 0x004FFFFFu)       return RGN_MCD212;
    if (a >= CDI_ROM_BASE   && a <  CDI_MCD212_BASE)    return RGN_ROM;
    if (a >= CDI_CDIC_BASE  && a <  0x00304000u)        return RGN_CDIC;
    if (a >= CDI_SLAVE_BASE && a <  0x0031001Eu)        return RGN_SLAVE;
    if (a >= CDI_NVRAM_BASE && a <  0x00324000u)        return RGN_NVRAM;
    if (a >= CDI_PERIPH_BASE && a <= CDI_PERIPH_LAST)   return RGN_PERIPH;
    return RGN_NONE;
}

/* ---- reads (big-endian) ---- */
uint8_t m68k_read8(uint32_t addr) {
    g_last_access_addr = addr;
    uint8_t *p = ram_ptr(addr);
    if (p) return *p;
    switch (classify(addr)) {
        case RGN_ROM:    return g_rom[addr - CDI_ROM_BASE];
        case RGN_MCD212: return (uint8_t)mcd212_read(addr, 1);
        case RGN_CDIC:   return (uint8_t)cdic_read(addr, 1);
        case RGN_SLAVE:  return (uint8_t)slave_read(addr, 1);
        case RGN_PERIPH: return (uint8_t)periph_read(addr, 1);
        default:         bus_fault("R", addr, 8); return 0;
    }
}
uint16_t m68k_read16(uint32_t addr) {
    g_last_access_addr = addr;
    uint8_t *p = ram_ptr(addr);
    if (p) return (uint16_t)((p[0] << 8) | p[1]);
    switch (classify(addr)) {
        case RGN_ROM:    { const uint8_t *r = &g_rom[addr - CDI_ROM_BASE];
                           return (uint16_t)((r[0] << 8) | r[1]); }
        case RGN_MCD212: return (uint16_t)mcd212_read(addr, 2);
        case RGN_CDIC:   return (uint16_t)cdic_read(addr, 2);
        case RGN_SLAVE:  return (uint16_t)slave_read(addr, 2);
        case RGN_PERIPH: return (uint16_t)periph_read(addr, 2);
        default:         bus_fault("R", addr, 16); return 0;
    }
}
uint32_t m68k_read32(uint32_t addr) {
    uint32_t v = ((uint32_t)m68k_read16(addr) << 16) | m68k_read16(addr + 2);
    g_last_access_addr = addr;   /* operand-base EA, not the +2 of the low word */
    return v;
}

/* ---- writes (big-endian) ---- */
/* When set, the store ring suppresses recording — used so a 32-bit write logs a
 * single 4-byte record rather than the two 16-bit writes it decomposes into. */
static int s_store_suppress = 0;

void m68k_write8(uint32_t addr, uint8_t val) {
    g_last_access_addr = addr;
    if (!s_store_suppress) debug_trace_store(addr, val, 1);
    uint8_t *p = ram_ptr(addr);
    if (p) { *p = val; return; }
    switch (classify(addr)) {
        case RGN_ROM:    return;  /* writes to ROM ignored */
        case RGN_MCD212: mcd212_write(addr, val, 1); return;
        case RGN_CDIC:   cdic_write(addr, val, 1);   return;
        case RGN_SLAVE:  slave_write(addr, val, 1);  return;
        case RGN_PERIPH: periph_write(addr, val, 1); return;
        default:         bus_fault("W", addr, 8);    return;
    }
}
void m68k_write16(uint32_t addr, uint16_t val) {
    g_last_access_addr = addr;
    if (!s_store_suppress) debug_trace_store(addr, val, 2);
    uint8_t *p = ram_ptr(addr);
    if (p) { p[0] = (uint8_t)(val >> 8); p[1] = (uint8_t)val; return; }
    switch (classify(addr)) {
        case RGN_ROM:    return;
        case RGN_MCD212: mcd212_write(addr, val, 2); return;
        case RGN_CDIC:   cdic_write(addr, val, 2);   return;
        case RGN_SLAVE:  slave_write(addr, val, 2);  return;
        case RGN_PERIPH: periph_write(addr, val, 2); return;
        default:         bus_fault("W", addr, 16);   return;
    }
}
void m68k_write32(uint32_t addr, uint32_t val) {
    s_store_suppress = 1;
    m68k_write16(addr,     (uint16_t)(val >> 16));
    m68k_write16(addr + 2, (uint16_t)val);
    s_store_suppress = 0;
    g_last_access_addr = addr;   /* operand-base EA, not the +2 of the low word */
    debug_trace_store(addr, val, 4);
}

/* Side-effect-free read for the debug surface: RAM and ROM only. MMIO and
 * unmapped addresses return 0 with a 0 result (a real read could change device
 * state, so the debugger must never route through the live accessors). */
int debug_peek8(uint32_t addr, uint8_t *out) {
    uint8_t *p = ram_ptr(addr);
    if (p) { *out = *p; return 1; }
    if (classify(addr) == RGN_ROM) { *out = g_rom[addr - CDI_ROM_BASE]; return 1; }
    *out = 0;
    return 0;
}
