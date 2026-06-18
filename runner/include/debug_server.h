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
