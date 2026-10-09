/*
 * main_cdi.c — CdiRecomp entry point.
 *
 * Usage: CdiRecomp <disc.cue|disc.bin> [--game <game.toml>]
 *                  [--emit --out <directory> [--module <name>]]
 * Inventories bounded filesystem modules and emits position-independent C.
 * The real CD-RTOS loader remains responsible for loading and linking them.
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
#include "module_emit.h"
#include "game_config.h"
/* Frontend headers — present so the shared 68000 pipeline links and is ready
 * to be driven once module->flat-image mapping exists. */
#include "rom_parser.h"
#include "function_finder.h"
#include "code_generator.h"
#include "codegen_diag.h"
#include "annotations.h"

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

static void usage(FILE *stream) {
    fprintf(stream,
        "Usage: CdiRecomp <disc.cue|disc.bin> [--game <game.toml>]\n"
        "                 [--emit --out <directory> [--module <name>]\n"
        "                  [--subr-exports named-offset32] [--module-seeds <file>]]\n"
        "\n"
        "Subroutine export layouts are explicit; named-offset32 is the verified\n"
        "Hotel Mario private layout. Module seeds contain image SHA-256 + offsets.\n"
        "Emission always rejects unsupported instructions and module falloff.\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        usage(stdout);
        return 0;
    }

    const char *disc_path = argv[1];
    const char *game_path = NULL;
    const char *output_path = NULL, *module_filter = NULL, *seed_path = NULL;
    bool emit = false,named_offset_pairs=false;
    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--game") && i + 1 < argc) game_path = argv[++i];
        else if (!strcmp(argv[i], "--emit"))                 emit = true;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) output_path = argv[++i];
        else if (!strcmp(argv[i], "--module") && i + 1 < argc) module_filter = argv[++i];
        else if (!strcmp(argv[i], "--module-seeds") && i + 1 < argc) seed_path = argv[++i];
        else if (!strcmp(argv[i], "--subr-exports") && i + 1 < argc &&
                 !strcmp(argv[i+1],"named-offset32")) { named_offset_pairs=true;i++; }
        else if (!strcmp(argv[i], "--fail-on-unsupported")) { /* always enforced */ }
        else { fprintf(stderr,"[CdiRecomp] unknown/incomplete option: %s\n",argv[i]);return 1; }
    }
    if (emit && !output_path) { fprintf(stderr,"[CdiRecomp] --emit requires --out <directory>\n");return 1; }

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
    if (n < 0) {
        fprintf(stderr, "[CdiRecomp] Invalid or unsupported disc filesystem\n");
        cdi_disc_close(&disc);
        return 1;
    }
    printf("[CdiRecomp] OS-9 modules with valid header parity: %d\n", n);
    int show = n < MAX_MODS ? n : MAX_MODS;
    for (int i = 0; i < show; i++) {
        printf("  [%3d] LBA %-7u size %-8u type=%-5s lang=%u crc=%s  %s  (%s+$%X)\n",
               i, mods[i].lba, mods[i].size, os9_type_name(mods[i].type),
               mods[i].lang, mods[i].crc_ok ? "ok" : "?", mods[i].name,
               mods[i].file_path, mods[i].file_offset);
    }
    if (n > show)
        printf("  ... (%d more not shown)\n", n - show);

    if (emit) {
        bool ok = cdi_emit_modules(&disc, mods, n, output_path, module_filter,named_offset_pairs,seed_path);
        cdi_disc_close(&disc);
        return ok ? 0 : 2;
    }
    cdi_disc_close(&disc);
    return 0;
}
