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

/* Per-launch identity echoed by ping. The co-sim supplies a random hex token
 * so an unrelated or stale process already holding the port cannot be
 * mistaken for the child that was just launched. Call before init. */
void debug_server_set_session(const char *session);

/* Legacy poll hook (the server is threaded now). Kept as a no-op so existing
 * call sites compile; remove once nothing calls it. */
void debug_server_poll(void);

/* Deterministic dev-only input playback. The comma-separated specification is
 * a strictly increasing list of completed-frame transitions (`frame:mask`).
 * A transition is published immediately after that frame completes, through
 * the same atomic host state consumed by the timed IKAT path. */
int  cdi_input_schedule_configure(const char *spec);
void cdi_input_schedule_advance(uint64_t completed_frame);

/* Capture one frame snapshot into the frame ring (call once per frame). */
void debug_ring_capture_frame(void);

/* Fold one executed MCD212 ICA/DCA word into the current completed-frame
 * diagnostic hash. area uses CeDImu's ControlArea order:
 * ICA1=0, DCA1=1, ICA2=2, DCA2=3. */
void debug_trace_mcd_event(uint8_t area, uint16_t line, uint32_t word);

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

/* Monotonic execution-ring position at the instruction currently in flight.
 * Device event rings snapshot this value so a hardware access can be joined
 * back to the full CPU trace without arming a future capture. */
uint64_t debug_trace_sequence(void);

/* Always-on CIAP access ring.  The CIAP model owns the records because reads
 * may have device side effects; the debug server only takes a side-effect-free
 * snapshot for the `ciap_events` TCP query. */
typedef struct {
    uint64_t seq;          /* CIAP event sequence */
    uint64_t trace_seq;    /* execution-ring sequence in flight */
    uint64_t frame;
    uint64_t cycles;
    uint32_t pc;
    uint32_t offset;
    uint32_t value;
    uint8_t  size;
    uint8_t  write;
} CdiCiapEvent;
int cdic_debug_events(CdiCiapEvent *out, int capacity, uint64_t from,
                      uint64_t *total, uint64_t *oldest);

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

/* ---- Uncovered-entry collection (trace-guided discovery) ----
 * Every control-flow entry absent from both the exact function table and async
 * native resume map is recorded here. That means register-indirect JSR/JMP
 * targets and only those depth-zero exception resumes that reveal a genuinely
 * undiscovered CFG; ordinary interior resumes are generated automatically.
 * Dump via `indirect_targets`, union in-ROM entries into the discovery seed
 * file, and re-seed until the new-CFG set is dry. */
void debug_record_indirect_target(uint32_t addr);

/* --fault-hold: freeze at a fatal fault instead of aborting, keeping the rings
 * queryable for first-divergence. `g_hold_on_fault` gates it; cdi_fault_hold()
 * parks the faulting thread (the server thread keeps serving). */
extern int g_hold_on_fault;
void cdi_fault_hold(void);

/* Freeze the run (rings intact) once this many blocks have been traced. 0 =
 * disabled. Set from --stop-seq N; deterministic stop for diffing a window the
 * (now non-faulting) boot would otherwise run past and evict. */
extern uint64_t g_stop_seq;

/* Frame-domain counterpart to --stop-seq.  The MCD212 parks immediately after
 * publishing this completed field, preserving the frame and all rings for a
 * side-effect-free native/oracle comparison. */
extern uint64_t g_stop_frame;
