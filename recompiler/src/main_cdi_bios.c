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
#include "annotations.h"
#include "game_config.h"
#include "cycle_probe.h"
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

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: CdiRecompBios <bios.rom> [--emit] [--game <cfg>]\n");
        return 1;
    }
    const char *rom_path = argv[1];
    const char *game_path = NULL;
    bool emit = false;
    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--emit"))                 emit = true;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game_path = argv[++i];
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
    if (!cfg.output_prefix[0])
        snprintf(cfg.output_prefix, sizeof cfg.output_prefix, "cdrtos");

    /* Seed discovery from every code module's execution entry. OS-9 modules are
     * relocatable but ROMmed modules execute in place; M$Exec (offset 0x30 in
     * the header, per CeDImu's ModuleExtraHeader) is the entry offset relative
     * to the module start. lang==1 means 68000 object code. The reset vector is
     * already seeded by the finder; these add the kernel + drivers + shell. */
    static uint32_t seeds[1024];
    int nseeds = 0;
    for (int i = 0; i < n && nseeds < 1024; i++) {
        if (mods[i].lang != 1) continue;
        uint32_t maddr = CDI_BIOS_ROM_BASE + (uint32_t)mods[i].logical_offset;
        if ((uint64_t)maddr + 0x34 > img_size) continue;
        const uint8_t *mh = img + maddr;
        uint32_t m_exec = ((uint32_t)mh[0x30] << 24) | ((uint32_t)mh[0x31] << 16) |
                          ((uint32_t)mh[0x32] << 8)  |  mh[0x33];
        if (m_exec == 0 || m_exec >= mods[i].size) continue;  /* entry within module */
        seeds[nseeds++] = maddr + m_exec;
    }
    if (nseeds > 0 && cfg.extra_func_count == 0) {
        cfg.extra_funcs      = seeds;
        cfg.extra_func_count = nseeds;
        printf("[CdiRecompBios] seeded %d module M$Exec entry points\n", nseeds);
    }

    if (cycle_probe_init(&rom) == 0)
        printf("[CdiRecompBios] cycle probe armed (clown68000)\n");
    else
        fprintf(stderr, "[CdiRecompBios] cycle probe init failed; PRM cycle fallback\n");

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
    const char *out_full = "bios/generated/cdrtos_full.c";
    const char *out_disp = "bios/generated/cdrtos_dispatch.c";
    bool ok = codegen_emit(&rom, &funcs, out_full, out_disp, &at, &cfg, false);

    codegen_diag_print_summary(stderr);
    int diag = codegen_diag_total();
    printf("[CdiRecompBios] codegen: %s | unsupported events: %d\n",
           ok ? "ok" : "FAILED", diag);
    printf("[CdiRecompBios] output: %s , %s\n", out_full, out_disp);

    cycle_probe_shutdown();
    function_list_free(&funcs);
    free(img);
    return ok ? 0 : 2;
}
