/*
 * main_cdi_bios.c — CdiRecompBios entry point.
 *
 * OS-FIRST strategy (mirrors psxrecomp): the CD-i CD-RTOS system ROM is a flat
 * 68000 ROM that boots from its reset vector. We statically recompile that ROM
 * (kernel + drivers + file managers + player shell) as native C, BEFORE any
 * game work. This tool ingests the ROM, inventories its OS-9 modules, and with
 * --emit drives the shared 68000 frontend (function_finder + code_generator)
 * over it.
 *
 * Address model (no frontend changes needed): the ROM lives at $400000 (verified
 * from the reset PC). We build an address-indexed image where the byte at
 * absolute address A sits at buffer offset A — ROM copied to offset $400000, and
 * the 68000 vector table mirrored to offset 0 (exactly the hardware reset alias,
 * so the finder seeds the reset/exception vectors as designed). Every existing
 * rom_data[addr] / addr<rom_size check in the frontend then works unchanged.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "disc_parser.h"
#include "rom_parser.h"
#include "function_finder.h"
#include "code_generator.h"
#include "codegen_diag.h"
#include "m68k_decoder.h"
#include "m68k_validator.h"
#include "annotations.h"
#include "game_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CD-i system ROM is permanently mapped here (reset PC of the CDI 490/220 ROMs
 * is $004004B8 = base + $4B8). At reset the hardware aliases ROM to $000000 so
 * the 68000 reads the vector table. */
#define CDI_BIOS_ROM_BASE 0x00400000u

static const char *os9_type_name(uint8_t t) {
    switch (t) {
        case 0x1: return "Prog";  case 0x2: return "Subr";  case 0x3: return "Multi";
        case 0x4: return "Data";  case 0xC: return "Sysm";  case 0xD: return "Fmgr";
        case 0xE: return "Drvr";  case 0xF: return "Devic"; default: return "?";
    }
}

/* Some CD-RTOS file managers expose each service through a position-
 * independent glue entry:
 *
 *   lea entry(pc),a2
 *   lea implementation(pc),a0
 *   bra common_dispatch
 *
 * The common dispatcher publishes A0 through an OS table and calls it later,
 * so ordinary direct-call discovery cannot see the implementation. Accept
 * only this exact, legal, same-module shape; treating arbitrary LEA targets as
 * code would turn strings and descriptor data into functions. */
static bool fmgr_glue_implementation(const GenesisRom *rom,
                                     uint32_t module_begin,
                                     uint32_t module_size,
                                     uint32_t entry,
                                     uint32_t *implementation) {
    M68KInstr ins[3];
    M68KValidatorOptions vopts = {0};
    uint32_t pc = entry;
    for (int i = 0; i < 3; i++) {
        if (!m68k_decode(rom, pc, &ins[i]) ||
            m68k_validate(&ins[i], &vopts) != M68K_LEGAL)
            return false;
        pc += ins[i].byte_length;
    }
    const uint8_t pc_disp = (uint8_t)((EA_PCR << 3) | PCR_PC_DISP);
    if (ins[0].mnemonic != MN_LEA || ins[0].reg != 2 ||
        ins[0].src_ea != pc_disp || ins[0].word_count < 2 ||
        ins[1].mnemonic != MN_LEA || ins[1].reg != 0 ||
        ins[1].src_ea != pc_disp || ins[1].word_count < 2 ||
        ins[2].mnemonic != MN_BRA || !ins[2].has_target)
        return false;

    uint32_t self = ins[0].addr + 2u +
                    (uint32_t)(int32_t)(int16_t)ins[0].words[1];
    uint32_t target = ins[1].addr + 2u +
                      (uint32_t)(int32_t)(int16_t)ins[1].words[1];
    uint64_t module_end = (uint64_t)module_begin + module_size;
    if (self != entry || target < module_begin || target >= module_end ||
        (target & 1u))
        return false;

    M68KInstr first;
    if (!m68k_decode(rom, target, &first) ||
        m68k_validate(&first, &vopts) != M68K_LEGAL)
        return false;
    *implementation = target;
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: CdiRecompBios <bios.rom> [--emit] [--game <cfg>] "
                        "[--seeds <file>] [--dump-functions <file>]\n");
        return 1;
    }
    const char *rom_path = argv[1];
    const char *game_path = NULL;
    const char *seeds_path = NULL;   /* flat hex list of extra discovery seeds */
    const char *dump_functions_path = NULL;
    bool emit = false;
    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--emit"))                  emit = true;
        else if (!strcmp(argv[i], "--game")  && i + 1 < argc) game_path  = argv[++i];
        else if (!strcmp(argv[i], "--seeds") && i + 1 < argc) seeds_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-functions") && i + 1 < argc)
            dump_functions_path = argv[++i];
    }

    /* ---- load ---- */
    FILE *f = fopen(rom_path, "rb");
    if (!f) { fprintf(stderr, "[bios] cannot open %s\n", rom_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 8) { fprintf(stderr, "[bios] ROM too small\n"); fclose(f); return 1; }
    uint8_t *raw = (uint8_t *)malloc((size_t)sz);
    if (!raw || fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "[bios] read failed\n"); free(raw); fclose(f); return 1;
    }
    fclose(f);

    uint32_t ssp = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
                   ((uint32_t)raw[2] << 8)  |  (uint32_t)raw[3];
    uint32_t pc  = ((uint32_t)raw[4] << 24) | ((uint32_t)raw[5] << 16) |
                   ((uint32_t)raw[6] << 8)  |  (uint32_t)raw[7];

    printf("[CdiRecompBios] ROM: %s  (%ld KB)\n", rom_path, sz / 1024);
    printf("[CdiRecompBios] reset SSP=$%08X  reset PC=$%08X  (ROM base $%08X)\n",
           ssp, pc, CDI_BIOS_ROM_BASE);

    /* ---- OS-9 module inventory ---- */
    enum { MAX_MODS = 1024 };
    static Os9Module mods[MAX_MODS];
    int n = os9_scan_buffer(raw, (uint64_t)sz, mods, MAX_MODS);
    printf("[CdiRecompBios] OS-9 modules (valid header parity): %d\n", n);
    for (int i = 0; i < n && i < MAX_MODS; i++)
        printf("  [%3d] $%08X  size %-7u type=%-5s lang=%u  %s\n",
               i, CDI_BIOS_ROM_BASE + (uint32_t)mods[i].logical_offset,
               mods[i].size, os9_type_name(mods[i].type), mods[i].lang, mods[i].name);

    if (!emit) { free(raw); return 0; }

    /* ---- build the address-indexed image ---- */
    uint64_t img_size = (uint64_t)CDI_BIOS_ROM_BASE + (uint64_t)sz;
    uint8_t *img = (uint8_t *)calloc(1, (size_t)img_size);
    if (!img) { fprintf(stderr, "[bios] OOM building %llu-byte image\n",
                        (unsigned long long)img_size); free(raw); return 1; }
    memcpy(img + CDI_BIOS_ROM_BASE, raw, (size_t)sz);          /* ROM at $400000 */
    /* Mirror ONLY the reset vector (SSP@0, PC@4) to $0. On CD-i the rest of the
     * 68000 vector table is built in RAM at boot, so ROM offsets 8.. are NOT real
     * vectors — mirroring them seeds bogus functions in the $0..$400000 dead zone. */
    memcpy(img, raw, 8);
    free(raw);

    GenesisRom rom;
    memset(&rom, 0, sizeof rom);
    rom.rom_data   = img;
    rom.rom_size   = (uint32_t)img_size;
    rom.initial_sp = ssp;
    rom.initial_pc = pc;

    GameConfig cfg = {0};
    if (!(game_path && game_config_load(&cfg, game_path)))
        game_config_init_empty(&cfg);
    /* CD-RTOS contains callable branch-entered suffixes and shared epilogues.
     * The conservative entry-ownership proof keeps every address dispatchable
     * while preventing those entries from truncating their canonical host. */
    cfg.function_aliases = true;
    /* IRQ/bus-error delivery unwinds generated host frames to a depth-zero
     * trampoline. The stacked PC may be any emitted instruction boundary, so
     * keep every such boundary natively resumable without splitting hosts. */
    cfg.async_resume_entries = true;
    /* OS-9 encodes every TRAP #0 call as `trap #0; dc.w service`. The kernel
     * advances the exception-frame PC over that selector, so keep the real
     * continuation in the caller's canonical CFG instead of manufacturing a
     * split function when the runtime first returns there. */
    cfg.trap0_inline_service_word = true;
    if (!cfg.output_prefix[0])
        snprintf(cfg.output_prefix, sizeof cfg.output_prefix, "cdrtos");

    /* Seed discovery from every 68000 module's declared execution entries.
     * OS-9 program/system-style M$Exec is a direct module-relative code offset.
     * File-manager and driver M$Exec instead points at their service vector.
     * The OS-9/68000 I/O ABI uses 13 word offsets relative to the beginning of
     * a file-manager table, but 7 word offsets relative to the module base for
     * a driver (the seventh is its optional error handler).
     * Treating that vector itself as instructions manufactures legal-looking
     * garbage functions (and hides real service routines). ROMmed modules run
     * in place, so every table row resolves relative to the module start. */
    #define CDI_SEED_MAX 16384
    static uint32_t seeds[CDI_SEED_MAX];
    int nseeds = 0;
    for (int i = 0; i < n && nseeds < CDI_SEED_MAX; i++) {
        if (mods[i].lang != 1) continue;
        uint32_t maddr = CDI_BIOS_ROM_BASE + (uint32_t)mods[i].logical_offset;
        if ((uint64_t)maddr + 0x34 > img_size) continue;
        const uint8_t *mh = img + maddr;
        uint32_t m_exec = ((uint32_t)mh[0x30] << 24) | ((uint32_t)mh[0x31] << 16) |
                          ((uint32_t)mh[0x32] << 8)  |  mh[0x33];
        if (m_exec == 0 || m_exec >= mods[i].size) continue;
        if (mods[i].type == 0x0D || mods[i].type == 0x0E) {
            int entries = mods[i].type == 0x0D ? 13 : 7;
            for (int e = 0; e < entries && nseeds < CDI_SEED_MAX; e++) {
                uint32_t row = m_exec + (uint32_t)e * 2u;
                if (row + 2u > mods[i].size) break;
                uint16_t off = ((uint16_t)mh[row] << 8) | mh[row + 1];
                uint32_t target = mods[i].type == 0x0D
                                ? m_exec + (uint32_t)off
                                : (uint32_t)off;
                if (off == 0 || target >= mods[i].size || (target & 1u)) continue;
                uint32_t entry = maddr + target;
                seeds[nseeds++] = entry;
                if (mods[i].type == 0x0D && nseeds < CDI_SEED_MAX) {
                    uint32_t implementation;
                    if (fmgr_glue_implementation(&rom, maddr, mods[i].size,
                                                 entry, &implementation))
                        seeds[nseeds++] = implementation;
                }
            }
        } else {
            seeds[nseeds++] = maddr + m_exec;
        }
    }
    int nmod_seeds = nseeds;

    /* Trace-guided discovery seeds: a flat hex list (one address per line, '#'
     * comments) of indirect-call targets the runtime observed the static finder
     * miss — register-indirect JSR (An) through RAM-built dispatch tables that
     * no static walk can follow, plus any resume that reveals a CFG not already
     * represented by the generated async native resume map. The runtime's
     * `indirect_targets` collection,
     * unioned by tools/collect_seeds.py into bios/cdrtos_discovered.txt, feeds
     * back here; re-seeding + regen recompiles them and exposes the next layer,
     * iterating until the miss set is dry. Explicit --seeds wins; otherwise the
     * default committed list is auto-loaded when present. Only in-ROM seeds are
     * kept (below-ROM RAM-resident copies are dropped downstream anyway). */
    {
        char default_seeds[256] = {0};
        const char *sp = seeds_path;
        if (!sp) {
            /* derive "<dir-of-rom>/cdrtos_discovered.txt" so it works regardless
             * of CWD; fall back to the literal path if no directory component. */
            const char *slash = strrchr(rom_path, '/');
            const char *bslash = strrchr(rom_path, '\\');
            const char *cut = slash > bslash ? slash : bslash;
            if (cut) {
                size_t dlen = (size_t)(cut - rom_path) + 1;
                if (dlen < sizeof default_seeds - 24) {
                    memcpy(default_seeds, rom_path, dlen);
                    strcpy(default_seeds + dlen, "cdrtos_discovered.txt");
                    sp = default_seeds;
                }
            } else {
                sp = "bios/cdrtos_discovered.txt";
            }
        }
        FILE *sf = sp ? fopen(sp, "r") : NULL;
        if (sf) {
            int added = 0, skipped_oob = 0;
            char ln[64];
            while (fgets(ln, sizeof ln, sf) && nseeds < CDI_SEED_MAX) {
                char *p = ln;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
                uint32_t a = (uint32_t)strtoul(p, NULL, 16) & 0xFFFFFFu;
                if (a < CDI_BIOS_ROM_BASE || (uint64_t)a >= img_size) { skipped_oob++; continue; }
                seeds[nseeds++] = a;
                added++;
            }
            fclose(sf);
            printf("[CdiRecompBios] seed file '%s': +%d in-ROM seeds (%d out-of-ROM skipped)\n",
                   sp, added, skipped_oob);
        } else if (seeds_path) {
            fprintf(stderr, "[CdiRecompBios] WARNING: --seeds '%s' unreadable\n", seeds_path);
        }
    }

    if (nseeds > 0 && cfg.extra_func_count == 0) {
        cfg.extra_funcs      = seeds;
        cfg.extra_func_count = nseeds;
        printf("[CdiRecompBios] seeded %d entry points (%d module M$Exec + %d trace-guided)\n",
               nseeds, nmod_seeds, nseeds - nmod_seeds);
    }

    static FunctionList funcs = {0};
    function_finder_run(&rom, &funcs, &cfg);
    printf("[CdiRecompBios] functions discovered from reset entry $%08X: %d\n",
           pc, funcs.count);

    /* Classify discovered functions by where they live. CD-RTOS boot copies
     * code into RAM and runs it there, so many control-flow targets land below
     * the ROM base in our ROM-only image — those need the module-relocation
     * model, not a ROM address. Report the split, then drop out-of-ROM entries
     * so codegen doesn't walk the dead zone. */
    {
        int in_rom = 0, below = 0;
        for (int i = 0; i < funcs.count; i++)
            (funcs.entries[i].addr >= CDI_BIOS_ROM_BASE) ? in_rom++ : below++;
        printf("[CdiRecompBios] %d functions: %d in-ROM (>=$%08X), %d below-ROM (RAM-resident?)\n",
               funcs.count, in_rom, CDI_BIOS_ROM_BASE, below);
        int show = funcs.count < 32 ? funcs.count : 32;
        for (int i = 0; i < show; i++)
            printf("    $%08X%s\n", funcs.entries[i].addr,
                   funcs.entries[i].addr < CDI_BIOS_ROM_BASE ? "   <- below ROM" : "");
        int kept = 0;
        for (int i = 0; i < funcs.count; i++)
            if (funcs.entries[i].addr >= CDI_BIOS_ROM_BASE)
                funcs.entries[kept++] = funcs.entries[i];
        funcs.count = kept;
    }

    AnnotationTable at = {0};
    if (dump_functions_path)
        codegen_set_dump_functions_path(dump_functions_path);
    const char *out_full = "bios/generated/cdrtos_full.c";
    const char *out_disp = "bios/generated/cdrtos_dispatch.c";
    bool ok = codegen_emit(&rom, &funcs, out_full, out_disp, &at, &cfg, false);

    codegen_diag_print_summary(stderr);
    int diag = codegen_diag_total();
    printf("[CdiRecompBios] codegen: %s | unsupported events: %d\n",
           ok ? "ok" : "FAILED", diag);
    printf("[CdiRecompBios] output: %s , %s\n", out_full, out_disp);

    function_list_free(&funcs);
    free(img);
    return ok ? 0 : 2;
}
