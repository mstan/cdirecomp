/*
 * cdrtos.c — CD-RTOS / OS-9 system-call HLE gateway (skeleton).
 *
 * THIS IS THE DEFINING CHALLENGE OF CD-i RECOMPILATION. Unlike the Genesis
 * (bare-metal cartridge), a CD-i title runs on CD-RTOS, a real-time OS based on
 * Microware OS-9/68K v2.4. Programs reach the system through the `OS9` macro,
 * which assembles to `TRAP #0` followed by an inline service-code word. The
 * trap dispatcher (runtime.c) routes vector 0x20 here; we read the inline code,
 * advance PC past it, and either emulate the service (HLE) or — eventually —
 * dispatch into a recompiled CD-RTOS kernel (LLE). HLE is the pragmatic first
 * path: implement F$Load/F$Link/F$Fork/I$Read/etc. against our disc_parser +
 * a managed RAM heap.
 *
 * Reference HLE: external/CeDImu/src/CDI/OS9/SystemCalls.{hpp,cpp} and HLE/.
 * Service-code names below mirror that enumeration.
 *
 * Skeleton: identify + name the call, then FAIL LOUD. As Hotel Mario boots,
 * each abort tells us exactly which OS-9 service to implement next — the
 * dominant-class-first triage the recompiler discipline asks for.
 */
#include "cdi_runtime.h"
#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>

static const char *os9_call_name(uint16_t code) {
    switch (code) {
        /* F$ — system/process services */
        case 0x0000: return "F$Link";
        case 0x0001: return "F$Load";
        case 0x0002: return "F$UnLink";
        case 0x0003: return "F$Fork";
        case 0x0004: return "F$Wait";
        case 0x0005: return "F$Chain";
        case 0x0006: return "F$Exit";
        case 0x0007: return "F$Mem";
        case 0x0008: return "F$Send";
        case 0x0009: return "F$Icpt";
        case 0x000A: return "F$Sleep";
        case 0x000C: return "F$ID";
        case 0x000D: return "F$SPrior";
        case 0x0015: return "F$Time";
        case 0x0016: return "F$STime";
        case 0x0017: return "F$CRC";
        case 0x001B: return "F$CpyMem";
        case 0x0028: return "F$SRqMem";
        case 0x0029: return "F$SRtMem";
        case 0x002A: return "F$IRQ";
        case 0x002C: return "F$AProc";
        case 0x002D: return "F$NProc";
        case 0x002E: return "F$VModul";
        case 0x0032: return "F$SSvc";
        /* I$ — I/O services */
        case 0x0080: return "I$Attach";
        case 0x0081: return "I$Detach";
        case 0x0082: return "I$Dup";
        case 0x0083: return "I$Create";
        case 0x0084: return "I$Open";
        case 0x0085: return "I$MakDir";
        case 0x0086: return "I$ChgDir";
        case 0x0087: return "I$Delete";
        case 0x0088: return "I$Seek";
        case 0x0089: return "I$Read";
        case 0x008A: return "I$Write";
        case 0x008B: return "I$ReadLn";
        case 0x008C: return "I$WritLn";
        case 0x008D: return "I$GetStt";
        case 0x008E: return "I$SetStt";
        case 0x008F: return "I$Close";
        default:     return "?";
    }
}

void cdrtos_syscall(void) {
    /* OS-9/68000: the service code is the inline word immediately after the
     * TRAP #0. The recompiled flow lands here with PC pointing at that word. */
    uint16_t code = m68k_read16(g_cpu.PC);
    g_cpu.PC += 2;

    fprintf(stderr,
        "[cdrtos] OS-9 call %s (0x%04X)  D0=$%08X D1=$%08X A0=$%08X A1=$%08X\n"
        "         CD-RTOS HLE not implemented yet — see TODO.md MC-CDI-001.\n",
        os9_call_name(code), code, g_cpu.D[0], g_cpu.D[1], g_cpu.A[0], g_cpu.A[1]);
    debug_dump_fault_trail("OS-9 TRAP #0 (HLE not implemented)");
    if (g_hold_on_fault) cdi_fault_hold();   /* --fault-hold: freeze rings-intact for diffing */
    abort();
}
