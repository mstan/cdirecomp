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
#include "cosim_state.h"
#include "cdi_frontend.h"
#include "cdi_media.h"
#include "cdi_host_time.h"
#include "player_config.h"
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
    /* Unbuffer stdout/stderr: a fail-loud fprintf followed by cdi_fault_hold()
     * (which blocks) or abort() would otherwise lose its message when output is
     * redirected to a file/pipe (fully buffered, never flushed). The runtime is
     * a debug tool — immediate, ordered diagnostics matter more than throughput. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    const char *rom_path = NULL;
    const char *disc_path = NULL;
    int port = 4380;   /* native; optional development oracle on +1 — see TCP.md */
    int hold = 0;      /* keep the rings queryable after the run ends */
    int exit_on_stop = 0; /* diagnostic one-shot; normal runtime keeps devices live */
    int headless = 0;  /* deterministic tools and unattended environments */
    int player_quit = 0;
    /* MC-CDI-016 (COSIM-SPEC.md §5): parsed unconditionally (even when built
     * with CDI_COSIM off) so the flag always correctly consumes its argument
     * — otherwise the spec string would fall through to the `argv[i][0] !=
     * '-'` branch below and get mistaken for rom_path. */
    const char *cosim_inject_spec = NULL;
    const char *cosim_session = "";
    const char *input_script_spec = NULL;
    CdiPlayerConfig player_config;
    char player_config_path[1024] = "";
    char nvram_path[1024] = "";
    int player_preferences_active;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold")) hold = 1;
        else if (!strcmp(argv[i], "--exit-on-stop")) exit_on_stop = 1;
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc_path = argv[++i];
        else if (!strcmp(argv[i], "--fault-hold")) g_hold_on_fault = 1;
        else if (!strcmp(argv[i], "--stop-seq") && i + 1 < argc) g_stop_seq = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--stop-frame") && i + 1 < argc) g_stop_frame = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--cosim-inject") && i + 1 < argc) cosim_inject_spec = argv[++i];
        else if (!strcmp(argv[i], "--cosim-session") && i + 1 < argc) cosim_session = argv[++i];
        else if (!strcmp(argv[i], "--input-script") && i + 1 < argc) input_script_spec = argv[++i];
        else if (argv[i][0] != '-') rom_path = argv[i];
    }

    printf("CdiRuntime — Philips CD-i (SCC68070) static-recomp runtime\n");
    if (!rom_path) {
        fprintf(stderr, "usage: CdiRuntime <cdrtos.rom> [--disc image.cue] [--port N] [--hold] [--headless] [--exit-on-stop] [--stop-seq N] [--stop-frame N]\n"
                        "                  [--cosim-inject <seq>:<ram|reg>:<idx>:<xorhex>]\n"
                        "                  [--input-script <frame:mask,...>]\n"
                        "  boots the recompiled CD-RTOS system ROM.\n"
                        "  --hold: keep the process (and the debug rings) alive after\n"
                        "          the run for post-mortem TCP inspection.\n"
                        "  --exit-on-stop: diagnostic one-shot; exit at the first STOP\n"
                        "          instead of advancing devices until an interrupt.\n"
                        "  --headless: disable the SDL display and physical input frontend.\n"
                        "  --disc: insert a single-track Mode-2 CUE/BIN image at power-on.\n"
                        "  --cosim-inject: MC-CDI-016 fault injection (see COSIM-SPEC.md\n"
                        "          §5); also settable via env CDI_COSIM_INJECT.\n"
                        "  --input-script: deterministic dev input transitions applied\n"
                        "          immediately after each named completed frame.\n");
        return 1;
    }

    cdi_player_config_defaults(&player_config);
    player_preferences_active = !headless && !g_stop_seq && !g_stop_frame &&
                                !input_script_spec && !cosim_inject_spec;
    if (player_preferences_active) {
        int loaded;
        const char *config_override = getenv("CDI_PLAYER_CONFIG_PATH");
        if (config_override && *config_override) {
            if (strlen(config_override) >= sizeof player_config_path) {
                fprintf(stderr, "[config] CDI_PLAYER_CONFIG_PATH is too long\n");
                return 1;
            }
            strcpy(player_config_path, config_override);
        } else {
            if (!cdi_player_config_default_path(player_config_path,
                                                sizeof player_config_path)) {
                fprintf(stderr, "[config] cannot determine the user preference path\n");
                return 1;
            }
        }
        loaded = cdi_player_config_load(player_config_path, &player_config);
        if (loaded < 0) return 1;
        if (loaded == 0 &&
            !cdi_player_config_save(player_config_path, &player_config)) {
            fprintf(stderr, "[config] cannot create %s\n", player_config_path);
            return 1;
        }
        printf("[config] %s (capture_mouse=%s, rtc_sync=%s)\n",
               player_config_path,
               player_config.capture_mouse ? "on" : "off",
               player_config.sync_host_on_startup ? "on" : "off");
        if (!cdi_player_config_sibling_path(player_config_path, "nvram.bin",
                                            nvram_path, sizeof nvram_path)) {
            fprintf(stderr, "[nvram] cannot form the battery-file path\n");
            return 1;
        }
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

    cdi_media_init();
    if (disc_path && !cdi_media_mount(disc_path)) return 1;

    printf("  ROM: %s (%ld KB) @ $%08X\n", rom_path, sz / 1024, CDI_ROM_BASE);
    printf("  reset SSP=$%08X  reset PC=$%08X\n", reset_ssp, reset_pc);

    /* ---- seed SCC68070 reset state ---- */
    runtime_init();                 /* zeroes g_cpu */
    periph_reset();                 /* on-chip peripheral power-on state (UART TxRDY) */
    nvram_reset();                  /* DS1216 SmartWatch: SRAM=$FF, clock=1989-01-01 */
    if (player_preferences_active) {
        int loaded = nvram_load_sram(nvram_path);
        if (loaded < 0) {
            fprintf(stderr,
                    "[nvram] %s must be an exact 32768-byte battery image\n",
                    nvram_path);
            return 1;
        }
        printf("[nvram] %s (%s)\n", nvram_path,
               loaded ? "loaded" : "new battery");
    }
    if (player_preferences_active && player_config.sync_host_on_startup) {
        CdiRtcTime host_time;
        int seeded;
        if (!cdi_host_local_time(&host_time)) {
            fprintf(stderr, "[rtc] cannot sample host-local civil time\n");
            return 1;
        }
        seeded = nvram_seed_clock_once(&host_time);
        if (seeded != 1) {
            fprintf(stderr,
                    "[rtc] host-local time is outside the DS1216 range: "
                    "%04d-%02u-%02u %02u:%02u:%02u\n",
                    host_time.year, host_time.month, host_time.date,
                    host_time.hour, host_time.minute, host_time.second);
            return 1;
        }
        printf("[rtc] startup seed %04d-%02u-%02u %02u:%02u:%02u.%02u "
               "(host local; one shot)\n",
               host_time.year, host_time.month, host_time.date,
               host_time.hour, host_time.minute, host_time.second,
               host_time.hundredth);
    }
    cdi_input_reset();
    mcd212_video_reset();           /* display parameters + canonical buffers */
    g_cpu.A[7] = reset_ssp;         /* supervisor stack pointer (S=1 → A7 aliases SSP) */
    g_cpu.SSP  = reset_ssp;         /* SSP shadow (used when a user->super swap restores A7) */
    g_cpu.PC   = reset_pc;
    g_cpu.SR   = 0x2700;            /* 68000 reset: S=1, IPL=7, T=0 */
    g_recomp_initial_ssp = reset_ssp;
    /* Player runs follow emulated time in real time. Fixed-sequence co-sim is
     * observational and must free-run deterministically. */
    runtime_set_realtime_pacing(g_stop_seq == 0 && g_stop_frame == 0);

#ifdef CDI_COSIM
    /* MC-CDI-016: reset RAM page-hash bookkeeping to all-dirty now that
     * g_ram0/g_ram1 are in their final reset state (zeroed; ROM load never
     * touches RAM) and BEFORE the first debug_trace_block() capture. */
    cdi_cosim_init();
    /* --cosim-inject wins over the env var when both are given. */
    if (!cosim_inject_spec) cosim_inject_spec = getenv("CDI_COSIM_INJECT");
    if (cosim_inject_spec) cdi_cosim_set_inject(cosim_inject_spec);
#endif

    debug_server_set_session(cosim_session);
    debug_server_init(port);
    if (input_script_spec && !cdi_input_schedule_configure(input_script_spec)) {
        fprintf(stderr, "[cdi] malformed --input-script '%s' (want frame:mask,...; frames strictly increasing)\n",
                input_script_spec);
        return 1;
    }

    /* Fixed-sequence co-simulation is an observational tool, not a player run;
     * suppress presentation automatically so it cannot add host scheduling
     * noise. Normal execution opens the physical frontend unless requested
     * otherwise. */
    int frontend_active = !headless && !g_stop_seq && !g_stop_frame;
    if (frontend_active &&
        !cdi_frontend_init(player_config.capture_mouse)) return 1;

    /* ---- drive execution from the reset entry ----
     * recomp_call_addr looks the entry up in the generated dispatch table and
     * runs it. With no device model yet this will fail loud at the first MMIO
     * touch (or log a dispatch miss); that abort/log IS the result we want —
     * the first thing the recompiled OS needs from the hardware. */
    printf("[cdi] entering recompiled CD-RTOS at $%08X ...\n", reset_pc);
    fflush(stdout);

    /* ---- top-level trampoline (MC-CDI-012) ----
     * The flat-call model returns control up the C stack on RTS, but the OS-9
     * boot does NOT bottom out at the reset entry — it transitions to the shell
     * via the dispatcher. Ways control reaches the loop body:
     *   (a) g_redirect_pending — a JSR site detected a rewritten return (the
     *       dispatcher resuming a different process) and unwound every C frame
     *       uncleared; re-dispatch at C-depth ~0 so the new context isn't nested
     *       inside the abandoned one. Also seeds the very first dispatch (the
     *       reset entry) and the bus-error handler dispatch (setjmp below).
     *   (b) a recompiled RTS bottomed out to main with the guest stack still
     *       holding a return address. That happens when a function was entered
     *       WITHOUT a mirroring C JSR (via the exception/dispatcher path), so no
     *       JSR site popped [A7]. The bare C `return` lands here while the guest
     *       wants to resume at [A7]. Pop it and dispatch — exactly what the guest
     *       RTS does. (This is the $406354 RTS -> $4040F0 wall.) A skip-RTS that
     *       set g_rte_pending also bubbles here; depth 0 is where that skip
     *       resolves — its return target is [A7] (g_cpu.PC is the RTS's own stale
     *       address), so follow [A7] and clear the flag before dispatching, or
     *       the fresh callee's first JSR site would wrongly see a return pending.
     * STOP enters a device-advance wait and resumes here on an eligible IRQ;
     * `--exit-on-stop` is the diagnostic exception. A no-progress guard fails
     * loud if ordinary guest dispatch stops advancing.
     *
     * The setjmp below is the recompiled-tier bus-error landing pad (MC-CDI-004):
     * an unmapped access from recompiled code longjmps here via g_recomp_bus_env
     * with the vector-2 frame already built and g_cpu.PC pointing at the OS-9
     * handler. The handler lives in RAM (the recompiler never saw it), so
     * dispatching g_cpu.PC routes through the hybrid interpreter, which runs the
     * handler, its RTE, and the resume in one shot before handing back to
     * recompiled code — no g_rte_pending plumbing needed here. (Variables read
     * after the longjmp are volatile per the setjmp/longjmp clobber rule.) */
    {
        volatile uint64_t last_insn = (uint64_t)-1;
        volatile unsigned stuck = 0;

        g_redirect_addr    = reset_pc;     /* first dispatch = the reset entry */
        g_redirect_pending = 1;

        g_recomp_bus_armed = 1;
        if (setjmp(g_recomp_bus_env) != 0) {
            /* Recompiled-tier bus error: frame built, g_cpu.PC = handler. */
            g_recomp_bus_armed = 1;
            g_redirect_addr    = g_cpu.PC & 0xFFFFFFu;
            g_redirect_pending = 1;
        }

        /* IRQ delivery landing pad (MC-CDI-007). recomp_take_irq() (called from
         * the per-instruction entry safepoint) builds the autovector frame,
         * points g_cpu.PC at the OS-9 handler, and longjmps here — abandoning the
         * recompiled DA-poll loop's C frame so the ISR does NOT run nested inside
         * it. Dispatch the handler at depth 0 like any other redirect; its RTE
         * pops the stacked resume PC and the loop follows [A7] back into the poll,
         * which now sees the event the ISR posted. Mirrors the bus-error pad. */
        g_recomp_irq_armed = 1;
        if (setjmp(g_recomp_irq_env) != 0) {
            g_recomp_bus_armed = 1;   /* both pads live for the next dispatch */
            g_recomp_irq_armed = 1;
            g_redirect_addr    = g_cpu.PC & 0xFFFFFFu;   /* = the interrupt handler */
            g_redirect_pending = 1;
        }

        for (;;) {
            uint32_t target;
            if (frontend_active && !cdi_frontend_pump()) {
                player_quit = 1;
                break;
            }
            if (g_redirect_pending) {
                g_redirect_pending = 0;
                target = g_redirect_addr;
            } else if (g_halted) {
                if (exit_on_stop) break;
                /* Service a stopped SCC68070 in 25-cycle device-time quanta.
                 * No instruction is traced while stopped. A device event raises
                 * g_irq_pending; the
                * standard IRQ path clears STOP, builds the autovector frame and
                 * longjmps to the landing pad above. */
                mcd212_tick(25);
                if (recomp_pending_irq_level()) recomp_take_irq();
                continue;
            } else if (g_rte_resume) {
                /* A recompiled RTE/RTR unwound to here: the instruction already popped
                 * SR/PC/format and g_cpu.PC holds the resume PC. Resume there —
                 * NOT at [A7] (that's the post-frame stack, not the return). */
                g_rte_resume = 0;
                target = g_cpu.PC & 0xFFFFFFu;
            } else {
                /* (skip-)RTS or dispatcher resume: follow the guest return on
                 * [A7]. A7 climbing above the boot SSP is NOT "guest done" — it
                 * is the OS-9 dispatcher switching to another task's (higher)
                 * stack (MOVE.L Dn,A7; push; RTS at $40637E/$406384). The OS
                 * never returns to nothing; only STOP (g_halted) or the
                 * no-progress guard ends this loop. (The old `A7 >= initial_ssp
                 * -> break` heuristic mistook that stack switch for the end of
                 * the boot and quit at ~seq 423000.) */
                target = m68k_read32(g_cpu.A[7]) & 0xFFFFFFu;
                g_cpu.A[7] += 4;
            }

            if (g_native_insn_count == last_insn) {
                if (++stuck > 100000u) {
                    fprintf(stderr, "[cdi] top-level trampoline made no progress over "
                            "%u dispatches (last target $%06X) — aborting.\n", stuck, target);
                    debug_dump_fault_trail("trampoline no-progress");
                    break;
                }
            } else {
                stuck = 0;
                last_insn = g_native_insn_count;
            }
            g_rte_pending = 0;   /* a fresh depth-0 dispatch never inherits a propagating return */
            g_rte_resume  = 0;   /* consumed above; never leak into the next dispatch */
            recomp_top_resume(target);
        }
        g_recomp_bus_armed = 0;
        g_recomp_irq_armed = 0;
    }

    if (player_quit)
        printf("[cdi] player requested shutdown after %llu instructions.\n",
               (unsigned long long)g_native_insn_count);
    else if (g_halted)
        printf("[cdi] CPU halted (STOP) after %llu instructions — shell idle reached. "
               "PC=$%08X SR=$%04X (--exit-on-stop).\n",
               (unsigned long long)g_native_insn_count, g_cpu.PC, g_cpu.SR);
    else
        printf("[cdi] top-level returned after %llu instructions "
               "(A7=$%08X) — investigate.\n",
               (unsigned long long)g_native_insn_count, g_cpu.A[7]);
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
    if (frontend_active) cdi_frontend_shutdown();
    if (player_preferences_active && !nvram_save_sram(nvram_path)) {
        fprintf(stderr, "[nvram] cannot save battery image %s\n", nvram_path);
        return 1;
    }
    return 0;
}
