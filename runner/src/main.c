/*
 * main.c — CdiRuntime entry point.
 *
 * OS-first boot: link the recompiled CD-RTOS system ROM (bios/generated/*.c),
 * load the ROM image into the bus, seed the SCC68070 reset state from the
 * reset vector, and drive execution from the reset entry through the generated
 * dispatch. The runtime fails loud on the first unmodelled device/region (per
 * the no-stub discipline), so a boot run reports exactly how far the recompiled
 * OS got and what hardware it touched next — the first divergence to model.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cdi_runtime.h"
#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <time.h>
#endif

/* bus ROM loader (cdi_bus.c) */
void cdi_bus_load_rom(const uint8_t *src, uint32_t n);

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int main(int argc, char *argv[]) {
    const char *rom_path = NULL;
    int port = 4380;   /* native; oracle (CeDImu) on +1 — see TCP.md */
    int hold = 0;      /* keep the rings queryable after the run ends */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold")) hold = 1;
        else if (argv[i][0] != '-') rom_path = argv[i];
    }

    printf("CdiRuntime — Philips CD-i (SCC68070) static-recomp runtime\n");
    if (!rom_path) {
        fprintf(stderr, "usage: CdiRuntime <cdrtos.rom> [--port N] [--hold]\n"
                        "  boots the recompiled CD-RTOS system ROM.\n"
                        "  --hold: keep the process (and the debug rings) alive after\n"
                        "          the run for post-mortem TCP inspection.\n");
        return 1;
    }

    /* ---- load the system ROM ---- */
    FILE *f = fopen(rom_path, "rb");
    if (!f) { fprintf(stderr, "[cdi] cannot open %s\n", rom_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 8 || sz > (long)CDI_ROM_SIZE) {
        fprintf(stderr, "[cdi] bad ROM size %ld (window is %u)\n", sz, CDI_ROM_SIZE);
        fclose(f); return 1;
    }
    uint8_t *raw = (uint8_t *)malloc((size_t)sz);
    if (!raw || fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "[cdi] ROM read failed\n"); free(raw); fclose(f); return 1;
    }
    fclose(f);

    uint32_t reset_ssp = be32(&raw[0]);
    uint32_t reset_pc  = be32(&raw[4]);
    cdi_bus_load_rom(raw, (uint32_t)sz);
    free(raw);

    printf("  ROM: %s (%ld KB) @ $%08X\n", rom_path, sz / 1024, CDI_ROM_BASE);
    printf("  reset SSP=$%08X  reset PC=$%08X\n", reset_ssp, reset_pc);

    /* ---- seed SCC68070 reset state ---- */
    runtime_init();                 /* zeroes g_cpu */
    periph_reset();                 /* on-chip peripheral power-on state (UART TxRDY) */
    g_cpu.A[7] = reset_ssp;         /* supervisor stack pointer */
    g_cpu.PC   = reset_pc;
    g_cpu.SR   = 0x2700;            /* 68000 reset: S=1, IPL=7, T=0 */
    g_recomp_initial_ssp = reset_ssp;

    debug_server_init(port);

    /* ---- drive execution from the reset entry ----
     * recomp_call_addr looks the entry up in the generated dispatch table and
     * runs it. With no device model yet this will fail loud at the first MMIO
     * touch (or log a dispatch miss); that abort/log IS the result we want —
     * the first thing the recompiled OS needs from the hardware. */
    printf("[cdi] entering recompiled CD-RTOS at $%08X ...\n", reset_pc);
    fflush(stdout);
    recomp_call_addr(reset_pc);

    /* If we get here the reset path returned without aborting — unexpected
     * this early; report honestly rather than imply a successful boot. */
    printf("[cdi] reset entry returned after %llu instructions "
           "(no abort) — unexpected; investigate.\n",
           (unsigned long long)g_native_insn_count);
    if (g_miss_count_any)
        printf("[cdi] %u dispatch miss(es); last at $%08X — RULE 0a: resolve "
               "before other debugging.\n", g_miss_count_any, g_miss_last_addr);

    if (hold) {
        printf("[cdi] --hold: rings live on :%d for inspection. Ctrl-C to exit.\n", port);
        fflush(stdout);
        for (;;) {
#ifdef _WIN32
            Sleep(1000);
#else
            struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL);
#endif
        }
    }
    return 0;
}
