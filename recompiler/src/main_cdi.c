/*
 * main_cdi.c — CdiRecomp entry point.
 *
 * Usage: CdiRecomp <disc.cue|disc.bin> [--game <game.toml>] [--emit]
 *                  [--reverse-debug] [--fail-on-unsupported]
 *
 * Today this inventories a CD-i disc image: track mode, volume descriptor, and
 * the OS-9/68000 modules that make up the program. The shared 68000 frontend
 * (function_finder + code_generator, copied verbatim from segagenesisrecomp) is
 * compiled and linked in, but CD-i code generation is gated behind the
 * CD-RTOS loader model (relocating OS-9 modules into a flat 68070 image), which
 * is the next milestone — see TODO.md (MC-CDI-001).
 *
 * NOTE: main_genesis.c sits next to this file as the unmodified upstream
 * reference for how the frontend is normally driven; it is not compiled.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "disc_parser.h"
#include "game_config.h"
/* Frontend headers — present so the shared 68000 pipeline links and is ready
 * to be driven once module->flat-image mapping exists. */
#include "rom_parser.h"
#include "function_finder.h"
#include "code_generator.h"
#include "codegen_diag.h"
#include "annotations.h"
#include "cycle_probe.h"

static const char *os9_type_name(uint8_t t) {
    switch (t) {
        case 0x1: return "Prog";
        case 0x2: return "Subr";
        case 0x3: return "Multi";
        case 0x4: return "Data";
        case 0xC: return "Sysm";
        case 0xD: return "Fmgr";
        case 0xE: return "Drvr";
        case 0xF: return "Devic";
        default:  return "?";
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: CdiRecomp <disc.cue|disc.bin> [--game <game.toml>] [--emit]\n"
            "                 [--reverse-debug] [--fail-on-unsupported]\n");
        return 1;
    }

    const char *disc_path = argv[1];
    const char *game_path = NULL;
    bool emit = false, reverse_debug = false, fail_on_unsupported = false;
    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--game") && i + 1 < argc) game_path = argv[++i];
        else if (!strcmp(argv[i], "--emit"))                 emit = true;
        else if (!strcmp(argv[i], "--reverse-debug"))        reverse_debug = true;
        else if (!strcmp(argv[i], "--fail-on-unsupported"))  fail_on_unsupported = true;
    }
    (void)reverse_debug; (void)fail_on_unsupported;

    printf("[CdiRecomp] Disc: %s\n", disc_path);
    CdiDisc disc;
    if (!cdi_disc_open(disc_path, &disc)) {
        fprintf(stderr, "[CdiRecomp] Failed to open disc image\n");
        return 1;
    }
    printf("[CdiRecomp] Image: %s\n", disc.bin_path);
    printf("[CdiRecomp] Size : %llu bytes  (%u sectors @2352, track MODE%d)\n",
           (unsigned long long)disc.bin_size, disc.sector_count, disc.track_mode);

    cdi_read_volume_descriptor(&disc);

    if (game_path) {
        GameConfig cfg = {0};
        if (game_config_load(&cfg, game_path))
            printf("[CdiRecomp] Game config: %s (prefix='%s')\n", game_path, cfg.output_prefix);
        else
            fprintf(stderr, "[CdiRecomp] Warning: could not load game config '%s'\n", game_path);
    }

    enum { MAX_MODS = 512 };
    static Os9Module mods[MAX_MODS];
    int n = cdi_scan_os9_modules(&disc, mods, MAX_MODS);
    printf("[CdiRecomp] OS-9 modules with valid header parity: %d\n", n);
    int show = n < MAX_MODS ? n : MAX_MODS;
    for (int i = 0; i < show; i++) {
        printf("  [%3d] LBA %-7u size %-8u type=%-5s lang=%u crc=%s  %s\n",
               i, mods[i].lba, mods[i].size, os9_type_name(mods[i].type),
               mods[i].lang, mods[i].crc_ok ? "ok" : "?", mods[i].name);
    }
    if (n > show)
        printf("  ... (%d more not shown)\n", n - show);

    cdi_disc_close(&disc);

    if (emit) {
        fprintf(stderr,
            "\n[CdiRecomp] --emit requested, but OS-9 module -> flat 68070 image\n"
            "  mapping is not implemented yet. The 68000 frontend builds and is\n"
            "  ready; CD-i needs the CD-RTOS loader model (module relocation +\n"
            "  base address) before code generation is meaningful.\n"
            "  See TODO.md: MC-CDI-001 (loader) / MC-CDI-004 (memory model).\n");
        return 3;
    }
    return 0;
}
