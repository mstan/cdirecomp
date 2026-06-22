/*
 * debug_server.h — runtime-internal debug surface (NOT part of the generated-
 * code contract in cdi_runtime.h). The always-on ring buffers + the TCP command
 * server live behind this. See TCP.md for the wire protocol and DEBUG.md for how
 * the rings are meant to be used (query backward; never arm-then-run).
 */
#pragma once
#include <stdint.h>

/* Start the threaded TCP command server on `port` and arm the rings. Safe to
 * call once at boot; the server answers queries on a background thread while the
 * main thread runs the recompiled OS. */
void debug_server_init(int port);

/* Legacy poll hook (the server is threaded now). Kept as a no-op so existing
 * call sites compile; remove once nothing calls it. */
void debug_server_poll(void);

/* Capture one frame snapshot into the frame ring (call once per frame). */
void debug_ring_capture_frame(void);

/* Capture one execution point (PC + full register file) into the block-trace
 * ring. Called from the per-block hook (glue_check_vblank) — always on. */
void debug_trace_block(void);

/* Dump the tail of the block-trace ring to stderr: the executed path into a
 * fault. Call from abort sites (unmapped bus access, illegal opcode, …) so the
 * crash report shows HOW we got there, not just the final registers. */
void debug_dump_fault_trail(const char *reason);

/* Always-on store ring: one record per guest memory WRITE (PC, addr, value,
 * size, block seq). Lets a memory-divergence be chased to its writer — query
 * "last write covering address A before seq S" rather than re-running. Called
 * from the bus write path (cdi_bus.c). `size` is 1/2/4 bytes. */
void debug_trace_store(uint32_t addr, uint32_t val, int size);

/* ---- Interpreter-fallback classification (always-on) ----
 * The hybrid interpreter (MC-CDI-011) runs whatever the static recompiler did
 * not cover. "Interpreter ran X% of boot" is useless without knowing WHICH PCs
 * and WHY — so every interpreted instruction is aggregated by PC and tagged
 * with the reason it fell through and its address region. The query answers the
 * architecture question (is the interpreter a safety net or becoming the
 * runtime?) and surfaces content-hash-promotion candidates. Same discipline as
 * the trace/store rings: always recording, queried after the fact.
 *
 * `g_fallback_reason` is set by the runtime at each interpreter entry and read
 * by debug_trace_interp() on the hot path. Recompiled (native) execution never
 * calls debug_trace_interp, so its value between interpreter runs is irrelevant. */
enum {
    FB_NONE = 0,          /* reason not set (should be rare; counts as a smell) */
    FB_DISPATCH_MISS,     /* generated dispatch found no function for the target */
    FB_TOP_RESUME,        /* depth-0 trampoline target was not a recompiled entry */
    FB_BUS_HANDLER,       /* OS-9 bus-error handler vectored from the recompiled tier */
    FB_EXCEPTION,         /* other exception / interrupt handler path (MC-CDI-007) */
    FB_REASON_COUNT
};
extern int g_fallback_reason;

/* Hot path: one call per interpreted instruction (from m68k_interp_step), after
 * the trace-ring sample. Aggregates by PC + g_fallback_reason + region. */
void debug_trace_interp(uint32_t pc);

/* --fault-hold: freeze at a fatal fault instead of aborting, keeping the rings
 * queryable for first-divergence. `g_hold_on_fault` gates it; cdi_fault_hold()
 * parks the faulting thread (the server thread keeps serving). */
extern int g_hold_on_fault;
void cdi_fault_hold(void);

/* Freeze the run (rings intact) once this many blocks have been traced. 0 =
 * disabled. Set from --stop-seq N; deterministic stop for diffing a window the
 * (now non-faulting) boot would otherwise run past and evict. */
extern uint64_t g_stop_seq;
