/*
 * debug_server.c — always-on ring buffer + TCP debug surface (skeleton).
 *
 * The recompiler discipline mandates an always-on ring buffer that captures
 * every relevant event continuously, queried after the fact — never an
 * arm-trace-then-run probe. This file owns that ring and the TCP command
 * server that exposes it. Native build listens on 4380, oracle (CeDImu) on
 * 4381 (native even? — we follow the +1 convention; see TCP.md).
 *
 * Skeleton: the frame ring buffer storage + API exist; the socket server and
 * the bulk of commands are TODO MC-CDI-015. State that clearly rather than
 * pretending coverage we don't have.
 */
#include "cdi_runtime.h"
#include <stdio.h>
#include <string.h>

#define CDI_FRAME_RING_LEN 36000   /* ~10 min @ 60 Hz, matching the NES/Genesis surface */

typedef struct {
    uint64_t frame;
    M68KState cpu;
    /* TODO MC-CDI-015: MCD212 plane/region regs, CDIC state, SLAVE input,
     * a WRAM snapshot, last recomp function name. */
} CdiFrameRecord;

static CdiFrameRecord s_ring[CDI_FRAME_RING_LEN];
static uint32_t s_write_idx = 0;
static int s_started = 0;

/* Record one frame snapshot into the ring (called once per frame by main loop). */
void debug_ring_capture_frame(void) {
    CdiFrameRecord *r = &s_ring[s_write_idx % CDI_FRAME_RING_LEN];
    r->frame = g_frame_count;
    r->cpu   = g_cpu;
    s_write_idx++;
}

/* Start the TCP command server on `port`. */
void debug_server_init(int port) {
    s_started = 1;
    fprintf(stderr, "[dbg] ring buffer armed (%d frames). TCP server on :%d is "
                    "TODO MC-CDI-015 (command surface specified in TCP.md).\n",
            CDI_FRAME_RING_LEN, port);
}

void debug_server_poll(void) {
    if (!s_started) return;
    /* TODO MC-CDI-015: accept one client, parse line/JSON commands, answer
     * from the ring buffer. */
}
