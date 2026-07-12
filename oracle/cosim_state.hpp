/*
 * cosim_state.hpp — oracle-side (CeDImu/C++) differential co-simulation state
 * hashing (MC-CDI-016). Single source of truth for the wire format is
 * docs/COSIM-SPEC.md; this header + cosim_state.cpp are the ORACLE half of
 * "one logical spec split across two files" (COSIM-SPEC.md §8) — the native
 * side is runner/src/cosim_state.c + runner/include/cosim_state.h (C). Same
 * algorithm, same constants, same exclusions — MUST stay byte-identical
 * across both.
 *
 * Gated entirely behind CDI_COSIM (see oracle/CMakeLists.txt: option
 * CDI_COSIM_BUILD, default ON, defines CDI_COSIM=1 for BOTH the CdiOracle
 * target and the vendored CeDImu library target). When CDI_COSIM is
 * undefined this header declares nothing, so cosim_state.cpp compiles to an
 * empty translation unit — safe to always list in the CMake source set.
 * Every call site elsewhere (cdi_oracle.cpp, the CeDImu vendored RAM-write
 * hook in cores/MCD212/DRAMInterface.cpp) wraps its calls into this API in
 * `#ifdef CDI_COSIM` for the same reason.
 */
#ifndef CDI_ORACLE_COSIM_STATE_HPP
#define CDI_ORACLE_COSIM_STATE_HPP

#include <cstdint>

#ifdef CDI_COSIM

class CDI;

/* Bind the two RAM bank backing arrays and mark every page dirty (forces a
 * full hash on the first cdi_cosim_ram_hash() call per bank). Call once,
 * after `cdi` is fully constructed and BEFORE the first capture()/step. */
void cdi_cosim_state_init(CDI *cdi);

/* Mark the page(s) touched by an `nbytes`-byte write at guest physical
 * address `phys` dirty. Call from the CeDImu RAM-write choke point (see
 * cores/MCD212/DRAMInterface.cpp, gated #ifdef CDI_COSIM there). No-op for
 * addresses outside the two RAM banks (bank0 0x00000..0x7FFFF, bank1
 * 0x200000..0x27FFFF) and before cdi_cosim_state_init() has run. */
void cdi_cosim_note_ram_write(uint32_t phys, uint32_t nbytes);

/* Per-seq incremental hash (§3a): recompute only pages marked dirty since the
 * last call, clear their dirty flag, then fold page_hash[0..127] in index
 * order. bank 0 folds the §8 canary first; bank 1 does not. bank must be 0 or
 * 1. Mutates the cached page_hash/page_dirty state (that IS the incremental
 * cache). */
uint64_t cdi_cosim_ram_hash(int bank);

/* Full from-scratch recompute (§5 cosim_full_ram_hash / Gate 4): hashes every
 * one of the 128 pages fresh from live memory and folds them the same way as
 * cdi_cosim_ram_hash, but does NOT read or mutate the incremental
 * page_hash/page_dirty cache — a from-scratch value that, compared against
 * the ring's incremental ram0_h/ram1_h at the same seq, proves the dirty-page
 * tracking (i.e. every RAM-write call site is hooked) is complete. */
uint64_t cdi_cosim_full_ram_hash(int bank);

/* Fault injection primitives (COSIM-SPEC.md §5, Gate 3). Applied to LIVE
 * state — the caller (capture(), in cdi_oracle.cpp) must apply these BEFORE
 * the state for the current seq is captured/hashed so the hasher observes
 * the flip immediately, not one seq late. */
void cdi_cosim_inject_ram(uint32_t addr, uint8_t xor_val); /* XOR the byte at guest addr `addr`; no-op if not RAM (ROM/MMIO excluded, §2b). Persists (marks the page dirty). */
void cdi_cosim_inject_reg(int idx, uint32_t xor_val);      /* idx: 0..7=D0..D7, 8..15=A0..A7, 16=USP, 17=SSP; XORs the full 32-bit register. No-op for an out-of-range idx. */

#endif /* CDI_COSIM */

#endif // CDI_ORACLE_COSIM_STATE_HPP
