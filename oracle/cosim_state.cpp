/*
 * cosim_state.cpp — oracle-side (CeDImu/C++) implementation of the COSIM-SPEC
 * §3 hash + §5/§7 fault-injection surface. MUST stay byte-identical to
 * runner/src/cosim_state.c (native/C side) in algorithm: same FNV-1a 64
 * constants, same 4096-byte pages, same 128 pages/bank, same hash-of-hashes
 * fold order, same §8 canary. See docs/COSIM-SPEC.md — the single source of
 * truth; if you change anything here, change the spec and the native side
 * together.
 *
 * The whole TU is gated behind CDI_COSIM (see oracle/CMakeLists.txt) so it
 * compiles to nothing — and costs nothing — when the instrument is built out
 * (CDI_COSIM_BUILD OFF), mirroring runner/src/cosim_state.c exactly.
 */
#include "cosim_state.hpp"

#ifdef CDI_COSIM

#include "CDI.hpp"
#include "cores/SCC68070/SCC68070.hpp"

#include <cstring>

namespace {

/* ---- §3: FNV-1a 64, exact constants ---- */
constexpr uint64_t FNV_OFFSET_BASIS = 0x14650FB0739D0383ULL;
constexpr uint64_t FNV_PRIME        = 0x00000100000001B3ULL;

/* ---- §3a: page geometry ---- */
constexpr uint32_t PAGE_SIZE       = 4096;
constexpr uint32_t PAGES_PER_BANK  = 128;
constexpr uint32_t BANK_SIZE       = PAGE_SIZE * PAGES_PER_BANK; /* 0x80000 */
constexpr uint32_t BANK1_PHYS_BASE = 0x200000;

/* ---- §8: canary, folded first into ram0_h only ---- */
constexpr uint64_t CANARY = 0xC0517EC0517EC051ULL;

uint64_t fnv1a(uint64_t h, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= FNV_PRIME;
    }
    return h;
}

struct Bank {
    uint8_t *base = nullptr;                    /* stable pointer into CeDImu's backing store */
    uint64_t page_hash[PAGES_PER_BANK] = {0};
    uint8_t  page_dirty[PAGES_PER_BANK] = {0};
};

Bank g_bank[2];
bool g_inited = false;
CDI *g_cdi_ref = nullptr;

/* Map a guest physical address to {bank, page}. Returns false for addresses
 * outside both RAM banks (ROM/MMIO/gap — correctly excluded per §2b). */
bool addr_to_bank_page(uint32_t phys, int *bank_out, uint32_t *page_out)
{
    if (phys < BANK_SIZE) {
        *bank_out = 0;
        *page_out = phys / PAGE_SIZE;
        return true;
    }
    if (phys >= BANK1_PHYS_BASE && phys < BANK1_PHYS_BASE + BANK_SIZE) {
        *bank_out = 1;
        *page_out = (phys - BANK1_PHYS_BASE) / PAGE_SIZE;
        return true;
    }
    return false;
}

/* Fold page_hash[0..127] (little-endian uint64_t bytes, index order) into a
 * single bank hash. `with_canary` folds the §8 canary (little-endian) first —
 * true for bank 0 (ram0_h) only, per spec: "folded first for ram0_h". */
uint64_t fold_page_hashes(const uint64_t hashes[PAGES_PER_BANK], bool with_canary)
{
    uint64_t h = FNV_OFFSET_BASIS;
    if (with_canary) {
        uint8_t canary_bytes[8];
        for (int i = 0; i < 8; i++) canary_bytes[i] = (uint8_t)(CANARY >> (8 * i));
        h = fnv1a(h, canary_bytes, sizeof canary_bytes);
    }
    uint8_t buf[PAGES_PER_BANK * 8];
    for (uint32_t p = 0; p < PAGES_PER_BANK; p++) {
        uint64_t v = hashes[p];
        for (int i = 0; i < 8; i++) buf[p * 8 + i] = (uint8_t)(v >> (8 * i));
    }
    return fnv1a(h, buf, sizeof buf);
}

} // namespace

void cdi_cosim_state_init(CDI *cdi)
{
    g_cdi_ref = cdi;

    /* GetRAMBank1()/GetRAMBank2() return RAMBank BY VALUE (a {span,base}
     * pair copied out of MCD212::GetRAMBank1/2), but the span's underlying
     * data pointer is stable: it wraps MCD212::m_memory, a single
     * std::vector<uint8_t> sized once at construction (0x280000 bytes) and
     * never resized afterward (verified: no other m_memory.resize/reserve
     * call exists in the CeDImu tree). CDI::GetPointer(addr) is the
     * documented stable-pointer accessor (its own header comment: "must not
     * be assumed to be consecutive with all the memory map" but says nothing
     * about instability within a bank) and is used here per the task's
     * fallback guidance, so this cache does not depend on RAMBank's span
     * surviving past the call that produced it. */
    const uint8_t *b0 = cdi->GetPointer(0x000000);
    const uint8_t *b1 = cdi->GetPointer(BANK1_PHYS_BASE);
    g_bank[0].base = const_cast<uint8_t *>(b0);
    g_bank[1].base = const_cast<uint8_t *>(b1);

    for (int b = 0; b < 2; b++) {
        memset(g_bank[b].page_hash, 0, sizeof g_bank[b].page_hash);
        memset(g_bank[b].page_dirty, 1, sizeof g_bank[b].page_dirty); /* all-dirty at init (§3a) */
    }
    g_inited = true;
}

void cdi_cosim_note_ram_write(uint32_t phys, uint32_t nbytes)
{
    if (!g_inited) return;
    /* nbytes is 1 (SetByte) or 2 (SetWord) in practice; loop is correct for
     * any width and cheap at these sizes. A straddling write marks both
     * pages it touches. */
    for (uint32_t i = 0; i < nbytes; i++) {
        int bank; uint32_t page;
        if (addr_to_bank_page(phys + i, &bank, &page)) g_bank[bank].page_dirty[page] = 1;
    }
}

uint64_t cdi_cosim_ram_hash(int bank)
{
    if (!g_inited || bank < 0 || bank > 1) return 0;
    Bank &bk = g_bank[bank];
    for (uint32_t p = 0; p < PAGES_PER_BANK; p++) {
        if (bk.page_dirty[p]) {
            bk.page_hash[p] = fnv1a(FNV_OFFSET_BASIS, bk.base + (size_t)p * PAGE_SIZE, PAGE_SIZE);
            bk.page_dirty[p] = 0;
        }
    }
    return fold_page_hashes(bk.page_hash, bank == 0);
}

uint64_t cdi_cosim_full_ram_hash(int bank)
{
    if (!g_inited || bank < 0 || bank > 1) return 0;
    Bank &bk = g_bank[bank];
    uint64_t local[PAGES_PER_BANK];
    for (uint32_t p = 0; p < PAGES_PER_BANK; p++)
        local[p] = fnv1a(FNV_OFFSET_BASIS, bk.base + (size_t)p * PAGE_SIZE, PAGE_SIZE);
    return fold_page_hashes(local, bank == 0);
}

/* ====================================================================== */
/*  Fault injection (COSIM-SPEC.md §5)                                    */
/* ====================================================================== */
void cdi_cosim_inject_ram(uint32_t addr, uint8_t xor_val)
{
    if (!g_inited) return;
    int bank; uint32_t page;
    if (!addr_to_bank_page(addr, &bank, &page)) return;
    uint32_t off = (bank == 0) ? addr : (addr - BANK1_PHYS_BASE);
    g_bank[bank].base[off] ^= xor_val;
    g_bank[bank].page_dirty[page] = 1; /* next cdi_cosim_ram_hash() picks it up, mirroring a real write */
}

void cdi_cosim_inject_reg(int idx, uint32_t xor_val)
{
    if (!g_cdi_ref) return;
    using Reg = SCC68070::Register;
    SCC68070 &cpu = g_cdi_ref->m_cpu;
    const auto regs = cpu.GetCPURegisters();

    if (idx >= 0 && idx <= 7) {
        Reg r = static_cast<Reg>(static_cast<int>(Reg::D0) + idx);
        cpu.SetRegister(r, regs.at(r) ^ xor_val);
    } else if (idx >= 8 && idx <= 15) {
        Reg r = static_cast<Reg>(static_cast<int>(Reg::A0) + (idx - 8));
        cpu.SetRegister(r, regs.at(r) ^ xor_val);
    } else if (idx == 16 || idx == 17) {
        /* USP (16) / SSP (17): SCC68070::SetRegister has a pre-existing bug
         * for these two cases — it assigns the CURRENT A(7) value instead of
         * the passed one ("case Register::USP: USP = A(7); break; // TODO:
         * this is wrong" in SCC68070.cpp), so it cannot be used directly and
         * we do not patch the vendored method (keeps the vendored-tree edit
         * footprint to the single RAM-write hook). Instead, route the write
         * through the A7 alias: SCC68070::A(7) resolves to SSP when S=1,
         * USP when S=0 (SCC68070.hpp), so flip S to select the target
         * shadow, write A7 through the public API, then restore SR exactly
         * (SetRegister(SR,...) is a plain field write with no side effects
         * beyond the S bit, so this is transparent to the running CPU). */
        const bool want_ssp = (idx == 17);
        Reg target = want_ssp ? Reg::SSP : Reg::USP;
        const uint16_t sr_before = static_cast<uint16_t>(regs.at(Reg::SR));
        const uint32_t cur = regs.at(target);
        const uint16_t sr_select = want_ssp ? (uint16_t)(sr_before | 0x2000) : (uint16_t)(sr_before & ~0x2000);

        cpu.SetRegister(Reg::SR, sr_select);
        cpu.SetRegister(Reg::A7, cur ^ xor_val);
        cpu.SetRegister(Reg::SR, sr_before);
    }
}

#endif /* CDI_COSIM */
