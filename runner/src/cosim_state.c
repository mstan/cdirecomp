/*
 * cosim_state.c — native-side differential co-simulation state hashing
 * (MC-CDI-016). See runner/include/cosim_state.h for the API contract and
 * docs/COSIM-SPEC.md for the wire-format spec this implements verbatim
 * (§3 hash algorithm, §3a incremental page-hash, §5 fault injection, §8
 * anti-blindness canary). The oracle side (CeDImu, C++) implements the
 * SAME algorithm/constants independently in oracle/cosim_state.cpp — the
 * two files are reviewed together and must stay byte-identical.
 *
 * The whole TU is gated behind CDI_COSIM (see runner/CMakeLists.txt) so it
 * compiles to nothing — and costs nothing — when the instrument is built
 * out (CDI_COSIM_BUILD OFF).
 */
#include "cdi_runtime.h"
#include "cosim_state.h"

#ifdef CDI_COSIM

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAM backing storage (defined in cdi_bus.c). Not exposed via cdi_runtime.h
 * (generated code never touches these directly — only via m68k_read/write),
 * so declare the extern locally here for the hasher's direct byte access. */
extern uint8_t g_ram0[CDI_RAM0_SIZE];
extern uint8_t g_ram1[CDI_RAM1_SIZE];

/* ====================================================================== */
/*  FNV-1a, 64-bit (COSIM-SPEC.md §3 — constants verbatim, MUST match the  */
/*  oracle side exactly).                                                 */
/* ====================================================================== */
#define COSIM_FNV_OFFSET_BASIS 0x14650FB0739D0383ULL
#define COSIM_FNV_PRIME        0x00000100000001B3ULL
#define COSIM_CANARY           0xC0517EC0517EC051ULL   /* §8 anti-blindness magic */

static uint64_t fnv1a_bytes(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= COSIM_FNV_PRIME;
    }
    return h;
}

/* Fold one 64-bit value into `h` as 8 little-endian bytes (§3a: "the array
 * is folded in index order 0->127 as little-endian uint64_t's"). */
static uint64_t fnv1a_fold_u64_le(uint64_t h, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) { b[i] = (uint8_t)(v & 0xFFu); v >>= 8; }
    return fnv1a_bytes(h, b, sizeof b);
}

/* ====================================================================== */
/*  Incremental page-hash, per bank (COSIM-SPEC.md §3a)                   */
/* ====================================================================== */
#define COSIM_PAGE_SIZE      4096u
#define COSIM_PAGES_PER_BANK 128u   /* 0x80000 / 4096 */

typedef struct {
    uint64_t page_hash [COSIM_PAGES_PER_BANK];
    uint8_t  page_dirty[COSIM_PAGES_PER_BANK];
} CosimBank;

static CosimBank s_bank[2];   /* [0] = RAM0 ($000000..$07FFFF), [1] = RAM1 ($200000..$27FFFF) */

static uint8_t  *bank_base(int bank) { return bank == 0 ? g_ram0 : g_ram1; }
static uint32_t  bank_phys(int bank) { return bank == 0 ? CDI_RAM0_BASE : CDI_RAM1_BASE; }
static uint32_t  bank_size(int bank) { return bank == 0 ? CDI_RAM0_SIZE : CDI_RAM1_SIZE; }

void cdi_cosim_init(void) {
    for (int b = 0; b < 2; b++) {
        memset(s_bank[b].page_hash, 0, sizeof s_bank[b].page_hash);
        /* all set at reset -> forces one full per-page hash on first use */
        memset(s_bank[b].page_dirty, 1, sizeof s_bank[b].page_dirty);
    }
}

static void mark_dirty_range(int bank, uint32_t off_start, uint32_t off_end_incl) {
    uint32_t p0 = off_start / COSIM_PAGE_SIZE;
    uint32_t p1 = off_end_incl / COSIM_PAGE_SIZE;
    if (p1 >= COSIM_PAGES_PER_BANK) p1 = COSIM_PAGES_PER_BANK - 1;
    for (uint32_t p = p0; p <= p1; p++)
        s_bank[bank].page_dirty[p] = 1;
}

void cdi_cosim_note_ram_write(uint32_t phys, uint32_t nbytes) {
    if (nbytes == 0) return;
    uint32_t last = phys + (nbytes - 1u);
    for (int b = 0; b < 2; b++) {
        uint32_t base = bank_phys(b), size = bank_size(b);
        if (phys - base < size) {                 /* same unsigned-wraparound idiom as cdi_bus.c ram_ptr() */
            uint32_t off0 = phys - base;
            uint32_t off1 = (last - base < size) ? (last - base) : (size - 1u);
            mark_dirty_range(b, off0, off1);
            return;
        }
    }
    /* Not RAM: callers gate on ram_ptr() != NULL (cdi_bus.c) or a validated
     * guest RAM address (cdi_cosim_inject_ram) before calling; this is
     * defensive only. */
}

/* Recompute + fold, from a caller-supplied page_hash array (either the live
 * incremental one after dirty pages are refreshed, or a scratch array built
 * fully fresh for the Gate-4 audit) — shared by cdi_cosim_ram_hash and
 * cdi_cosim_full_ram_hash so the two are byte-for-byte comparable. */
static uint64_t fold_bank_hash(int bank, const uint64_t *page_hash) {
    uint64_t h = COSIM_FNV_OFFSET_BASIS;
    if (bank == 0) h = fnv1a_fold_u64_le(h, COSIM_CANARY);   /* §8: canary folds first, ram0_h only */
    for (uint32_t p = 0; p < COSIM_PAGES_PER_BANK; p++)
        h = fnv1a_fold_u64_le(h, page_hash[p]);
    return h;
}

uint64_t cdi_cosim_ram_hash(int bank) {
    if (bank != 0 && bank != 1) return 0;
    CosimBank *bs = &s_bank[bank];
    const uint8_t *base = bank_base(bank);
    for (uint32_t p = 0; p < COSIM_PAGES_PER_BANK; p++) {
        if (bs->page_dirty[p]) {
            bs->page_hash[p] = fnv1a_bytes(COSIM_FNV_OFFSET_BASIS,
                                            base + (size_t)p * COSIM_PAGE_SIZE,
                                            COSIM_PAGE_SIZE);
            bs->page_dirty[p] = 0;
        }
    }
    return fold_bank_hash(bank, bs->page_hash);
}

uint64_t cdi_cosim_full_ram_hash(int bank) {
    if (bank != 0 && bank != 1) return 0;
    const uint8_t *base = bank_base(bank);
    uint64_t scratch[COSIM_PAGES_PER_BANK];
    for (uint32_t p = 0; p < COSIM_PAGES_PER_BANK; p++)
        scratch[p] = fnv1a_bytes(COSIM_FNV_OFFSET_BASIS,
                                  base + (size_t)p * COSIM_PAGE_SIZE,
                                  COSIM_PAGE_SIZE);
    /* Deliberately does NOT touch s_bank[bank] — this is the from-scratch
     * audit path, independent of the incremental dirty-tracking state. */
    return fold_bank_hash(bank, scratch);
}

/* ====================================================================== */
/*  Fault injection (COSIM-SPEC.md §5)                                    */
/* ====================================================================== */
void cdi_cosim_inject_ram(uint32_t addr, uint32_t xorval) {
    uint8_t *p = NULL;
    if (addr - CDI_RAM0_BASE < CDI_RAM0_SIZE)      p = &g_ram0[addr - CDI_RAM0_BASE];
    else if (addr - CDI_RAM1_BASE < CDI_RAM1_SIZE) p = &g_ram1[addr - CDI_RAM1_BASE];
    if (!p) return;   /* not RAM (ROM/MMIO excluded, §2b) — no-op */
    *p ^= (uint8_t)xorval;
    cdi_cosim_note_ram_write(addr, 1);   /* mark dirty so the NEXT hash call sees the flip */
}

void cdi_cosim_inject_reg(int idx, uint32_t xorval) {
    if (idx >= 0 && idx <= 7)       g_cpu.D[idx]     ^= xorval;
    else if (idx >= 8 && idx <= 15) g_cpu.A[idx - 8]  ^= xorval;
    /* MC-CDI-016 (COSIM-SPEC.md §2a): idx 16/17 must target the CANONICAL
     * USP/SSP location, not the raw shadow field — g_cpu.USP/.SSP hold the
     * INACTIVE shadow, so XORing the shadow while that role is ACTIVE
     * (live value in g_cpu.A[7]) would be invisible/stale against the now-
     * canonical emit and would not match how the oracle applies an inject
     * (it already targets its A7-alias). */
    else if (idx == 16) {  /* USP */
        if (g_cpu.SR & 0x2000u) g_cpu.USP  ^= xorval;   /* S=1: USP is the inactive shadow */
        else                    g_cpu.A[7] ^= xorval;   /* S=0: USP is active (== A7)      */
    }
    else if (idx == 17) {  /* SSP */
        if (g_cpu.SR & 0x2000u) g_cpu.A[7] ^= xorval;   /* S=1: SSP is active (== A7)      */
        else                    g_cpu.SSP  ^= xorval;   /* S=0: SSP is the inactive shadow */
    }
    /* else: out-of-range idx, no-op */
}

/* ---- --cosim-inject "<seq>:<kind>:<idx>:<xorhex>" parse + one-shot apply ---- */
enum { COSIM_INJECT_RAM = 0, COSIM_INJECT_REG = 1 };

static int      s_inject_have    = 0;
static int      s_inject_applied = 0;
static uint64_t s_inject_seq     = 0;
static int      s_inject_kind    = COSIM_INJECT_RAM;
static uint32_t s_inject_idx     = 0;   /* ram: guest addr; reg: register index */
static uint32_t s_inject_xor     = 0;

void cdi_cosim_set_inject(const char *spec) {
    if (!spec || !*spec) return;

    char *end;
    uint64_t seq = strtoull(spec, &end, 0);
    if (*end != ':') goto bad;
    const char *p = end + 1;

    char kind[8] = {0};
    int i = 0;
    while (*p && *p != ':' && i < (int)sizeof(kind) - 1) kind[i++] = *p++;
    kind[i] = 0;
    if (*p != ':') goto bad;
    p++;

    uint64_t idx = strtoull(p, &end, 0);
    if (*end != ':') goto bad;
    p = end + 1;

    uint64_t xorv = strtoull(p, &end, 16);

    int kind_code;
    if      (!strcmp(kind, "ram")) kind_code = COSIM_INJECT_RAM;
    else if (!strcmp(kind, "reg")) kind_code = COSIM_INJECT_REG;
    else goto bad;

    s_inject_seq     = seq;
    s_inject_kind    = kind_code;
    s_inject_idx     = (uint32_t)idx;
    s_inject_xor     = (uint32_t)xorv;
    s_inject_have    = 1;
    s_inject_applied = 0;
    fprintf(stderr, "[cosim] inject armed: seq=%llu kind=%s idx=%u xor=$%08X\n",
            (unsigned long long)seq, kind_code == COSIM_INJECT_REG ? "reg" : "ram",
            s_inject_idx, s_inject_xor);
    return;

bad:
    fprintf(stderr, "[cosim] --cosim-inject: malformed spec '%s' "
                    "(want <seq>:<ram|reg>:<idx>:<xorhex>) — ignored\n", spec);
}

void cdi_cosim_maybe_inject(uint64_t seq) {
    if (!s_inject_have || s_inject_applied) return;
    if (seq != s_inject_seq) return;
    s_inject_applied = 1;   /* set before applying: never re-armed by a re-entrant call */
    if (s_inject_kind == COSIM_INJECT_REG) {
        cdi_cosim_inject_reg((int)s_inject_idx, s_inject_xor);
        fprintf(stderr, "[cosim] fired reg inject at seq=%llu: idx=%u xor=$%08X\n",
                (unsigned long long)seq, s_inject_idx, s_inject_xor);
    } else {
        cdi_cosim_inject_ram(s_inject_idx, s_inject_xor);
        fprintf(stderr, "[cosim] fired ram inject at seq=%llu: addr=$%08X xor=$%02X\n",
                (unsigned long long)seq, s_inject_idx, s_inject_xor & 0xFFu);
    }
}

#endif /* CDI_COSIM */
