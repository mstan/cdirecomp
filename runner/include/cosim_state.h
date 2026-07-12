/*
 * cosim_state.h — native-side differential co-simulation state hashing
 * (MC-CDI-016). Single source of truth for the wire format is
 * docs/COSIM-SPEC.md; this header + its cosim_state.c are the NATIVE half
 * of "one logical spec split across two files" (COSIM-SPEC.md §8) — the
 * oracle side is oracle/cosim_state.cpp (CeDImu, C++). Same algorithm,
 * same constants, same exclusions — MUST stay byte-identical across both.
 *
 * Gated entirely behind CDI_COSIM (see runner/CMakeLists.txt: option
 * CDI_COSIM_BUILD, default ON, defines CDI_COSIM=1). When CDI_COSIM is
 * undefined this header declares nothing, so cosim_state.c compiles to an
 * empty translation unit — safe to always list in the CMake source set.
 * Every call site elsewhere (cdi_bus.c, debug_server.c, main.c) wraps its
 * calls into this API in `#ifdef CDI_COSIM` for the same reason.
 */
#pragma once
#include <stdint.h>

#ifdef CDI_COSIM

/* Reset both banks' page-hash bookkeeping to "all dirty" (forces one full
 * per-page recompute on the next cdi_cosim_ram_hash() call for each bank).
 * Call once at boot, after g_ram0/g_ram1 are in their reset state (zeroed)
 * and before the first debug_trace_block() capture. Does NOT touch fault-
 * injection state (cdi_cosim_set_inject is independent and may be called
 * before or after this). */
void cdi_cosim_init(void);

/* RAM write hook (COSIM-SPEC.md §3b). Mark dirty every 4096-byte page that
 * [phys, phys+nbytes) touches, in whichever bank (RAM0/RAM1) the range
 * falls in (handles a write straddling two pages). Call from the SAME
 * choke point on every guest RAM byte mutation: the RAM branch of
 * m68k_write8/m68k_write16 in cdi_bus.c (m68k_write32 decomposes into two
 * m68k_write16 calls, so hooking write16 covers it) — and from
 * cdi_cosim_inject_ram, which bypasses m68k_write* by design (fault
 * injection into LIVE state) but must still mark its own byte dirty. A
 * `phys` range outside both banks is a no-op. */
void cdi_cosim_note_ram_write(uint32_t phys, uint32_t nbytes);

/* Incremental bank hash (COSIM-SPEC.md §3a): recompute any dirty page's
 * FNV-1a page_hash (clearing its dirty flag), then fold page_hash[0..127]
 * into the bank hash in index order as little-endian uint64_t's. bank: 0 =
 * g_ram0 (RAM0), 1 = g_ram1 (RAM1). bank 0 additionally folds the §8
 * canary (0xC0517EC0517EC051) as the very first bytes, so ram0_h alone
 * detects a structural (byte-order/field-count) mismatch. O(dirty pages
 * since the last call) + one 1 KB fold — cheap enough for the per-seq hot
 * path in debug_trace_block(). */
uint64_t cdi_cosim_ram_hash(int bank);

/* Gate-4 audit (COSIM-SPEC.md §7.4): recompute the bank hash FROM SCRATCH
 * over live RAM, entirely ignoring the incremental page_hash/page_dirty
 * arrays (does not read or write them). Directly comparable to
 * cdi_cosim_ram_hash()'s return value at the same instant — a mismatch
 * means a RAM-mutating path exists that never called
 * cdi_cosim_note_ram_write (a missed write-hook site). */
uint64_t cdi_cosim_full_ram_hash(int bank);

/* Fault injection primitives (COSIM-SPEC.md §5, Gate 3). Applied to LIVE
 * state — the caller (cdi_cosim_maybe_inject, or a future TCP command)
 * must apply these BEFORE the state for the current seq is captured/hashed
 * so the hasher observes the flip immediately, not one seq late. */
void cdi_cosim_inject_ram(uint32_t addr, uint32_t xorval);  /* XOR the byte at guest addr `addr` by (uint8_t)xorval; persists (marks the page dirty). No-op if `addr` isn't RAM. */
void cdi_cosim_inject_reg(int idx, uint32_t xorval);         /* idx: 0..7=D0..D7, 8..15=A0..A7, 16=USP, 17=SSP; XORs the full 32-bit register. No-op for an out-of-range idx. */

/* --cosim-inject CLI flag / CDI_COSIM_INJECT env var (COSIM-SPEC.md §5).
 * Parse "<seq>:<kind>:<idx>:<xorhex>" (kind = "ram" | "reg"; seq/idx accept
 * decimal or 0x-hex, xorhex is parsed base-16) and arm a one-shot pending
 * injection. Malformed specs are reported to stderr and ignored (never
 * armed). Call once at startup (main.c), before the run starts. */
void cdi_cosim_set_inject(const char *spec);

/* Called from the per-instruction trace hook (debug_trace_block) with the
 * CURRENT seq, i.e. the seq about to be captured into the ring record. If a
 * pending injection (see cdi_cosim_set_inject) targets this exact seq and
 * hasn't fired yet, applies it now (exactly once) via
 * cdi_cosim_inject_ram/reg — so debug_trace_block's subsequent `r->cpu =
 * g_cpu` and cdi_cosim_ram_hash() calls for THIS seq already reflect the
 * flip, matching the "diff halts at seq K" behavior Gate 3 requires. No-op
 * once fired, or if no injection is armed. */
void cdi_cosim_maybe_inject(uint64_t seq);

#endif /* CDI_COSIM */
