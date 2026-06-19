/*
 * debug_server.c — always-on ring buffers + threaded TCP debug surface.
 *
 * The recompiler discipline (and the project's global rules) mandate ALWAYS-ON
 * ring buffers that record every relevant event continuously, queried after the
 * fact — never an arm-trace-then-run probe. This file owns:
 *
 *   - the block-trace ring : every executed block's {PC, full register file},
 *     captured from the per-block hook (glue_check_vblank). This is the trail
 *     that turns "it aborted at $FFFFFFFC" into "here are the 256 blocks that
 *     led there." Dumped on any fault (debug_dump_fault_trail).
 *   - the frame ring       : a per-frame snapshot (grows as the runtime grows).
 *   - a threaded TCP server : answers queries from the rings + live CPU state
 *     while the main thread runs. Native build listens on 4380, oracle on 4381
 *     (+1 convention; see TCP.md).
 *
 * Wire protocol: one command per line, '\n'-terminated. JSON object preferred
 * ({"cmd":"read_mem","addr":1024,"len":64}); a bare token (ping) is accepted for
 * the simplest commands. One single-line JSON response per request.
 */
#include "cdi_runtime.h"
#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET sock_t;
  #define BAD_SOCK INVALID_SOCKET
  #define close_sock closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <pthread.h>
  #include <time.h>
  typedef int sock_t;
  #define BAD_SOCK (-1)
  #define close_sock close
#endif

/* ====================================================================== */
/*  Frame ring (per-frame snapshot)                                       */
/* ====================================================================== */
#define CDI_FRAME_RING_LEN 36000   /* ~10 min @ 60 Hz, matching NES/Genesis */

typedef struct {
    uint64_t  frame;
    M68KState cpu;
    /* TODO MC-CDI-015: MCD212 plane/region regs, CDIC state, IKAT input,
     * a WRAM snapshot, last recomp function name. */
} CdiFrameRecord;

static CdiFrameRecord s_frame_ring[CDI_FRAME_RING_LEN];
static uint32_t s_frame_widx = 0;

void debug_ring_capture_frame(void) {
    CdiFrameRecord *r = &s_frame_ring[s_frame_widx % CDI_FRAME_RING_LEN];
    r->frame = g_frame_count;
    r->cpu   = g_cpu;
    s_frame_widx++;
}

/* ====================================================================== */
/*  Block-trace ring (every executed block: PC + full register file)      */
/* ====================================================================== */
#define CDI_TRACE_RING_LEN (1u << 18)   /* 262144 blocks (~20 MB) */
#define CDI_TRACE_MASK     (CDI_TRACE_RING_LEN - 1u)

typedef struct {
    uint64_t  seq;     /* monotonic block index since boot */
    M68KState cpu;     /* full register file AT block entry (PC live) */
    uint32_t  a7_top;  /* longword at [A7] (stack top) at block entry. Folded
                          into the realign alignment key so a caller / context-
                          switch split (identical registers but a different
                          pushed return address) can't hide behind register
                          alignment — and so fill/copy loops can be told apart
                          from in-place poll loops. */
} CdiTraceRecord;

static CdiTraceRecord s_trace_ring[CDI_TRACE_RING_LEN];
static uint64_t       s_trace_seq = 0;   /* number of blocks captured */

/* Side-effect-free big-endian longword read of guest memory. debug_peek8 lives
 * in cdi_bus.c and, unlike m68k_read, does NOT touch g_last_access_addr — so
 * sampling [A7] on the hot trace path can't perturb the address-error frame's
 * captured fault address. Unmapped bytes read back as 0. */
int debug_peek8(uint32_t addr, uint8_t *out);   /* cdi_bus.c */
static uint32_t debug_peek_be32(uint32_t addr) {
    uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    debug_peek8(addr,     &b0); debug_peek8(addr + 1, &b1);
    debug_peek8(addr + 2, &b2); debug_peek8(addr + 3, &b3);
    return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
}

/* When non-zero, freeze the run once this many blocks have been traced, leaving
 * the rings intact for diffing — the deterministic analogue of --fault-hold for
 * a boot that no longer faults (the bus error is now handled, so execution runs
 * past the window of interest and would otherwise evict it). Set via --stop-seq. */
uint64_t g_stop_seq = 0;

/* Hot path: one per executed block. Kept to a single struct copy. */
void debug_trace_block(void) {
    CdiTraceRecord *r = &s_trace_ring[s_trace_seq & CDI_TRACE_MASK];
    r->seq = s_trace_seq;
    r->cpu = g_cpu;
    r->a7_top = debug_peek_be32(g_cpu.A[7]);
    s_trace_seq++;
    if (g_stop_seq && s_trace_seq >= g_stop_seq) cdi_fault_hold();
}

/* Snapshot the most recent `count` trace records (oldest-first) into `out`.
 * Returns the number filled. Reads are racy vs the running main thread but a
 * snapshot is all a debugger needs. */
static int trace_tail(CdiTraceRecord *out, int count) {
    uint64_t total = s_trace_seq;
    if (count > CDI_TRACE_RING_LEN) count = CDI_TRACE_RING_LEN;
    if ((uint64_t)count > total) count = (int)total;
    uint64_t start = total - (uint64_t)count;
    for (int i = 0; i < count; i++)
        out[i] = s_trace_ring[(start + (uint64_t)i) & CDI_TRACE_MASK];
    return count;
}

/* Snapshot `count` records starting at sequence `from` (forward). Records older
 * than the ring window have been evicted and are skipped. Returns the number
 * filled; *first_seq receives the seq of out[0]. Used for first-divergence
 * paging from the start of the run (vs the tail). */
static int trace_range(CdiTraceRecord *out, uint64_t from, int count, uint64_t *first_seq) {
    uint64_t total = s_trace_seq;
    uint64_t oldest = total > CDI_TRACE_RING_LEN ? total - CDI_TRACE_RING_LEN : 0;
    if (from < oldest) from = oldest;
    if (count > CDI_TRACE_RING_LEN) count = CDI_TRACE_RING_LEN;
    int n = 0;
    for (uint64_t s = from; s < total && n < count; s++, n++)
        out[n] = s_trace_ring[s & CDI_TRACE_MASK];
    *first_seq = from;
    return n;
}

/* ====================================================================== */
/*  Store ring (every guest memory write: PC + addr + value + block seq)   */
/* ====================================================================== */
/* Always-on per-write log so a memory-divergence (a cell read back with the
 * wrong value) can be chased to its WRITER without re-running: query the most
 * recent store covering an address before a given seq. ~1M records (~16 MB). */
#define CDI_STORE_RING_LEN (1u << 20)
#define CDI_STORE_MASK     (CDI_STORE_RING_LEN - 1u)

typedef struct {
    uint64_t seq;      /* block seq in flight when the store happened */
    uint32_t pc;       /* g_cpu.PC at the store (the writing instruction) */
    uint32_t addr;     /* guest address written */
    uint32_t val;      /* value written (low `size` bytes meaningful) */
    uint8_t  size;     /* 1, 2, or 4 */
} CdiStoreRecord;

static CdiStoreRecord s_store_ring[CDI_STORE_RING_LEN];
static uint64_t       s_store_count = 0;

void debug_trace_store(uint32_t addr, uint32_t val, int size) {
    CdiStoreRecord *r = &s_store_ring[s_store_count & CDI_STORE_MASK];
    r->seq  = s_trace_seq;
    r->pc   = g_cpu.PC;
    r->addr = addr;
    r->val  = val;
    r->size = (uint8_t)size;
    s_store_count++;
}

/* ---- fault trail: dump the executed path into an abort ---- */
void debug_dump_fault_trail(const char *reason) {
    enum { N = 24 };
    static CdiTraceRecord tail[N];     /* static: avoid stack use during a fault */
    int n = trace_tail(tail, N);
    fprintf(stderr, "\n[fault-trail] %s — last %d executed blocks "
                    "(of %llu total):\n", reason ? reason : "fault", n,
            (unsigned long long)s_trace_seq);
    for (int i = 0; i < n; i++) {
        const M68KState *c = &tail[i].cpu;
        fprintf(stderr, "  #%llu  PC=$%08X  SR=$%04X  D0=$%08X A0=$%08X A6=$%08X A7=$%08X\n",
                (unsigned long long)tail[i].seq, c->PC, c->SR,
                c->D[0], c->A[0], c->A[6], c->A[7]);
    }
    fflush(stderr);
}

/* ====================================================================== */
/*  Side-effect-free memory peek (defined in cdi_bus.c)                    */
/* ====================================================================== */
int debug_peek8(uint32_t addr, uint8_t *out);   /* 1 = readable, 0 = MMIO/unmapped */

/* ====================================================================== */
/*  Tiny JSON-ish request parsing                                         */
/* ====================================================================== */
/* Extract the string value of "key":"value" into buf. Returns 1 on success. */
static int json_str(const char *s, const char *key, char *buf, int buflen) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(s, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < buflen - 1) buf[i++] = *p++;
    buf[i] = 0;
    return 1;
}
/* Extract the integer value of "key":N (decimal or 0x hex). Returns 1 on hit. */
static int json_int(const char *s, const char *key, uint64_t *out) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(s, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    *out = (uint64_t)strtoull(p, NULL, 0);
    return 1;
}
/* Resolve the command token: JSON "cmd":"x", else the first bare word. */
static void parse_cmd(const char *line, char *cmd, int cmdlen) {
    if (json_str(line, "cmd", cmd, cmdlen)) return;
    int i = 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && i < cmdlen - 1)
        cmd[i++] = *p++;
    cmd[i] = 0;
}

/* ====================================================================== */
/*  Command handlers — write a single-line JSON response into `out`        */
/* ====================================================================== */
static void resp_registers(char *out, int outlen) {
    int n = snprintf(out, outlen, "{\"ok\":true,\"pc\":%u,\"sr\":%u,\"usp\":%u",
                     g_cpu.PC, g_cpu.SR, g_cpu.USP);
    for (int i = 0; i < 8 && n < outlen; i++)
        n += snprintf(out + n, outlen - n, ",\"d%d\":%u", i, g_cpu.D[i]);
    for (int i = 0; i < 8 && n < outlen; i++)
        n += snprintf(out + n, outlen - n, ",\"a%d\":%u", i, g_cpu.A[i]);
    snprintf(out + n, outlen - n, "}");
}

static void resp_status(char *out, int outlen) {
    snprintf(out, outlen,
        "{\"ok\":true,\"insns\":%llu,\"blocks\":%llu,\"frame\":%llu,\"pc\":%u,"
        "\"miss_count\":%u,\"miss_last\":%u,\"irq_pending\":%u}",
        (unsigned long long)g_native_insn_count, (unsigned long long)s_trace_seq,
        (unsigned long long)g_frame_count, g_cpu.PC,
        g_miss_count_any, g_miss_last_addr, g_irq_pending);
}

static void resp_miss(char *out, int outlen) {
    int n = snprintf(out, outlen,
        "{\"ok\":true,\"count\":%u,\"last_addr\":%u,\"last_frame\":%llu,\"unique\":[",
        g_miss_count_any, g_miss_last_addr, (unsigned long long)g_miss_last_frame);
    for (int i = 0; i < g_miss_unique_count && n < outlen; i++)
        n += snprintf(out + n, outlen - n, "%s%u", i ? "," : "", g_miss_unique_addrs[i]);
    snprintf(out + n, outlen - n, "]}");
}

static void resp_read_mem(const char *line, char *out, int outlen) {
    uint64_t addr = 0, len = 16;
    if (!json_int(line, "addr", &addr)) { snprintf(out, outlen, "{\"ok\":false,\"error\":\"addr required\"}"); return; }
    json_int(line, "len", &len);
    if (len > 4096) len = 4096;
    int n = snprintf(out, outlen, "{\"ok\":true,\"addr\":%u,\"bytes\":\"", (uint32_t)addr);
    for (uint64_t i = 0; i < len && n < outlen - 4; i++) {
        uint8_t b;
        if (debug_peek8((uint32_t)(addr + i), &b)) n += snprintf(out + n, outlen - n, "%02X", b);
        else                                       n += snprintf(out + n, outlen - n, "--");
    }
    snprintf(out + n, outlen - n, "\"}");
}

static void resp_trace(const char *line, char *out, int outlen) {
    uint64_t count = 16, from = 0;
    json_int(line, "count", &count);
    if (count > 256) count = 256;
    static CdiTraceRecord recs[256];
    int got;
    uint64_t first = 0;
    int have_from = json_int(line, "from", &from);
    if (have_from) got = trace_range(recs, from, (int)count, &first);   /* forward from seq */
    else           got = trace_tail(recs, (int)count);                  /* most-recent tail */
    int n = snprintf(out, outlen, "{\"ok\":true,\"total\":%llu,\"records\":[",
                     (unsigned long long)s_trace_seq);
    for (int i = 0; i < got && n < outlen - 400; i++) {
        const M68KState *c = &recs[i].cpu;
        n += snprintf(out + n, outlen - n,
            "%s{\"seq\":%llu,\"pc\":%u,\"sr\":%u", i ? "," : "",
            (unsigned long long)recs[i].seq, c->PC, c->SR);
        for (int r = 0; r < 8; r++) n += snprintf(out + n, outlen - n, ",\"d%d\":%u", r, c->D[r]);
        for (int r = 0; r < 8; r++) n += snprintf(out + n, outlen - n, ",\"a%d\":%u", r, c->A[r]);
        n += snprintf(out + n, outlen - n, ",\"a7top\":%u", recs[i].a7_top);
        n += snprintf(out + n, outlen - n, "}");
    }
    snprintf(out + n, outlen - n, "]}");
}

/* stores: chase a memory-divergence to its writer. Params:
 *   addr   (required) the byte whose writers we want
 *   before (optional) only stores with seq < before (default: all)
 *   count  (optional) how many most-recent matches to return (default 16, max 64)
 * Returns matches oldest-first (so the LAST element is the most recent writer),
 * scanning the ring backward. A store matches if its [addr, addr+size) covers
 * the queried byte. */
static void resp_stores(const char *line, char *out, int outlen) {
    uint64_t addr = 0, before = 0, count = 16;
    if (!json_int(line, "addr", &addr)) { snprintf(out, outlen, "{\"ok\":false,\"error\":\"addr required\"}"); return; }
    int have_before = json_int(line, "before", &before);
    json_int(line, "count", &count);
    if (count > 64) count = 64;

    uint64_t total  = s_store_count;
    uint64_t oldest = total > CDI_STORE_RING_LEN ? total - CDI_STORE_RING_LEN : 0;
    static CdiStoreRecord hits[64];
    int nh = 0;
    /* scan backward; collect up to `count` most-recent covering stores */
    for (uint64_t i = total; i > oldest && nh < (int)count; ) {
        i--;
        const CdiStoreRecord *r = &s_store_ring[i & CDI_STORE_MASK];
        if (have_before && r->seq >= before) continue;
        if (addr >= r->addr && addr < (uint64_t)r->addr + r->size)
            hits[nh++] = *r;
    }
    int n = snprintf(out, outlen, "{\"ok\":true,\"addr\":%u,\"total\":%llu,\"oldest\":%llu,\"records\":[",
                     (uint32_t)addr, (unsigned long long)total, (unsigned long long)oldest);
    /* emit oldest-first (reverse of the backward scan) */
    for (int k = nh - 1; k >= 0 && n < outlen - 160; k--) {
        const CdiStoreRecord *r = &hits[k];
        n += snprintf(out + n, outlen - n,
            "%s{\"seq\":%llu,\"pc\":%u,\"addr\":%u,\"val\":%u,\"size\":%u}",
            k == nh - 1 ? "" : ",", (unsigned long long)r->seq, r->pc, r->addr, r->val, r->size);
    }
    snprintf(out + n, outlen - n, "]}");
}

/* Returns 1 to keep the connection open, 0 to close it (quit). */
static int handle_line(const char *line, char *out, int outlen) {
    char cmd[32];
    parse_cmd(line, cmd, sizeof cmd);

    if (!strcmp(cmd, "ping"))               snprintf(out, outlen, "{\"ok\":true,\"pong\":true}");
    else if (!strcmp(cmd, "status"))        resp_status(out, outlen);
    else if (!strcmp(cmd, "get_registers")) resp_registers(out, outlen);
    else if (!strcmp(cmd, "read_mem"))      resp_read_mem(line, out, outlen);
    else if (!strcmp(cmd, "trace"))         resp_trace(line, out, outlen);
    else if (!strcmp(cmd, "stores"))        resp_stores(line, out, outlen);
    else if (!strcmp(cmd, "dispatch_miss_info")) resp_miss(out, outlen);
    else if (!strcmp(cmd, "quit"))        { snprintf(out, outlen, "{\"ok\":true,\"bye\":true}"); return 0; }
    else snprintf(out, outlen, "{\"ok\":false,\"error\":\"unknown cmd '%s'\"}", cmd);
    return 1;
}

/* ====================================================================== */
/*  Threaded TCP server                                                   */
/* ====================================================================== */
static int s_port = 0;

static void serve_client(sock_t cs) {
    static char in[2048];
    static char out[65536];   /* static: full-register trace pages are large */
    int inlen = 0;
    for (;;) {
        char ch;
        int r = (int)recv(cs, &ch, 1, 0);
        if (r <= 0) break;
        if (ch == '\n') {
            in[inlen] = 0;
            int keep = handle_line(in, out, (int)sizeof out);
            int olen = (int)strlen(out);
            out[olen++] = '\n';
            send(cs, out, olen, 0);
            inlen = 0;
            if (!keep) break;
        } else if (ch != '\r' && inlen < (int)sizeof in - 1) {
            in[inlen++] = ch;
        }
    }
    close_sock(cs);
}

static void server_loop(void) {
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == BAD_SOCK) { fprintf(stderr, "[dbg] socket() failed\n"); return; }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)s_port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "[dbg] bind(:%d) failed — debug server off\n", s_port);
        close_sock(ls); return;
    }
    listen(ls, 1);
    fprintf(stderr, "[dbg] TCP debug server live on 127.0.0.1:%d "
                    "(ping/status/get_registers/read_mem/trace/dispatch_miss_info)\n", s_port);
    for (;;) {
        sock_t cs = accept(ls, NULL, NULL);
        if (cs == BAD_SOCK) break;
        serve_client(cs);   /* one client at a time, per TCP.md */
    }
    close_sock(ls);
}

#ifdef _WIN32
static DWORD WINAPI server_thread(LPVOID arg) { (void)arg; server_loop(); return 0; }
#else
static void *server_thread(void *arg) { (void)arg; server_loop(); return NULL; }
#endif

void debug_server_init(int port) {
    s_port = port;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[dbg] WSAStartup failed — debug server off\n");
        return;
    }
    HANDLE h = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
#else
    pthread_t t;
    pthread_create(&t, NULL, server_thread, NULL);
    pthread_detach(t);
#endif
    fprintf(stderr, "[dbg] rings armed: trace=%u blocks, frame=%u frames\n",
            CDI_TRACE_RING_LEN, CDI_FRAME_RING_LEN);
}

void debug_server_poll(void) { /* threaded now — nothing to poll */ }

/* When set (--fault-hold), a fatal fault freezes the process here instead of
 * aborting, so the always-on rings stay queryable at the fault point (the whole
 * boot trace is preserved for first-divergence). The server thread keeps
 * answering while the main thread is parked. */
int g_hold_on_fault = 0;

void cdi_fault_hold(void) {
    fprintf(stderr, "[fault-hold] frozen at fault; rings live on :%d for inspection. "
                    "Ctrl-C to exit.\n", s_port);
    fflush(stderr);
    for (;;) {
#ifdef _WIN32
        Sleep(1000);
#else
        struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL);
#endif
    }
}
