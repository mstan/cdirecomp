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
#include "cosim_state.h"
#include "cdi_media.h"
#include "cdi_audio.h"
#include "mcd212_video.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

/* Development-only detail kept out of the public renderer interface. */
void mcd212_video_debug_clut(uint32_t out[256]);

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

static char s_cosim_session[65];

void debug_server_set_session(const char *session) {
    snprintf(s_cosim_session, sizeof s_cosim_session, "%s", session ? session : "");
}

typedef struct {
    uint64_t  frame;
    M68KState cpu;
    uint16_t  width;
    uint16_t  height;
    uint64_t  argb_hash;
    uint32_t  mcd_count;
    uint64_t  mcd_hash;
    /* TODO MC-CDI-015: MCD212 plane/region regs, CDIC state, IKAT input,
     * a WRAM snapshot, last recomp function name. */
} CdiFrameRecord;

static CdiFrameRecord s_frame_ring[CDI_FRAME_RING_LEN];
static uint32_t s_frame_widx = 0;
static uint32_t s_mcd_count = 0;
static uint64_t s_mcd_hash = 0x14650FB0739D0383ULL;

void debug_trace_mcd_event(uint8_t area, uint16_t line, uint32_t word) {
    const uint64_t prime = 0x00000100000001B3ULL;
    const uint8_t bytes[7] = {
        area, (uint8_t)line, (uint8_t)(line >> 8),
        (uint8_t)word, (uint8_t)(word >> 8),
        (uint8_t)(word >> 16), (uint8_t)(word >> 24)
    };
    for (size_t i = 0; i < sizeof bytes; i++) {
        s_mcd_hash ^= bytes[i];
        s_mcd_hash *= prime;
    }
    s_mcd_count++;
}

void debug_ring_capture_frame(void) {
    CdiFrameRecord *r = &s_frame_ring[s_frame_widx % CDI_FRAME_RING_LEN];
    r->frame = g_frame_count;
    r->cpu   = g_cpu;
    r->argb_hash = mcd212_framebuffer_hash(&r->width, &r->height, NULL);
    r->mcd_count = s_mcd_count;
    r->mcd_hash = s_mcd_hash;
    s_mcd_count = 0;
    s_mcd_hash = 0x14650FB0739D0383ULL;
    s_frame_widx++;
}

/* ====================================================================== */
/*  Block-trace ring (every executed block: PC + full register file)      */
/* ====================================================================== */
#define CDI_TRACE_RING_LEN (1u << 20)   /* 1048576 blocks (~92 MB) — holds [1,~1M)
                                           from seq 1 so heavily-recompiled (coarse-
                                           native) builds can be re-aligned from the
                                           aligned start, where mid-stream realign of
                                           a low-interp stream is unreliable. Bumped
                                           1<<19 -> 1<<20: boot now reaches the IRQ
                                           frontier at ~seq 575k, past the old cap. */
#define CDI_TRACE_MASK     (CDI_TRACE_RING_LEN - 1u)

typedef struct {
    uint64_t  seq;     /* monotonic block index since boot */
    M68KState cpu;     /* full register file AT block entry (PC live) */
    uint16_t  opcode;  /* instruction word at cpu.PC, sampled at entry */
    uint64_t  total_cyc; /* g_total_cycles at block entry — diff vs CeDImu totalCycleCount
                            per seq to localize a sub-frame cycle drift (MC-CDI-009). */
    uint32_t  frame;   /* g_frame_count at block entry — diff display-timing vs
                          the oracle's GetTotalFrameCount() per seq, so a device-
                          clock drift (invisible to register alignment during a
                          spin loop) is localizable. (MC-CDI-007 timing.) */
    uint32_t  a7_top;  /* longword at [A7] (stack top) at block entry. Folded
                          into the realign alignment key so a caller / context-
                          switch split (identical registers but a different
                          pushed return address) can't hide behind register
                          alignment — and so fill/copy loops can be told apart
                          from in-place poll loops. */
    uint64_t  ram0_h;  /* MC-CDI-016: incremental FNV-1a hash of g_ram0 at block
                          entry (COSIM-SPEC.md §3). Only meaningfully populated
                          when built with CDI_COSIM (default ON); 0 otherwise. */
    uint64_t  ram1_h;  /* same, g_ram1. */
} CdiTraceRecord;

static CdiTraceRecord s_trace_ring[CDI_TRACE_RING_LEN];
static uint64_t       s_trace_seq = 0;   /* number of blocks captured */

uint64_t debug_trace_sequence(void) {
    return s_trace_seq;
}

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

static uint16_t debug_peek_be16(uint32_t addr) {
    uint8_t b0 = 0, b1 = 0;
    debug_peek8(addr, &b0); debug_peek8(addr + 1, &b1);
    return ((uint16_t)b0 << 8) | (uint16_t)b1;
}

/* When non-zero, freeze the run once this many blocks have been traced, leaving
 * the rings intact for diffing — the deterministic analogue of --fault-hold for
 * a boot that no longer faults (the bus error is now handled, so execution runs
 * past the window of interest and would otherwise evict it). Set via --stop-seq. */
uint64_t g_stop_seq = 0;
uint64_t g_stop_frame = 0;

/* Immediate debugger freeze requested by the TCP server. Unlike --stop-seq,
 * this is not an armed future capture: the rings have already been recording,
 * and `pause` asks the CPU thread to park at its very next traced instruction
 * so the just-observed history cannot be overwritten while it is queried. */
static atomic_int s_pause_requested;

/* Hot path: one per executed block. Kept to a single struct copy. */
void debug_trace_block(void) {
    /* IRQ delivery safepoint (MC-CDI-007). This runs at EVERY instruction ENTRY
     * in both tiers (the recompiled code emits debug_trace_block() after setting
     * g_cpu.PC; the interpreter calls it at the top of m68k_interp_step) — the
     * one universal per-instruction boundary with g_cpu.PC = the instruction
     * about to execute. If an unmasked interrupt is pending, recomp_take_irq()
     * builds the exception frame (resume PC = g_cpu.PC) and longjmps to the
     * trampoline WITHOUT returning — so the deferred instruction is neither
     * traced nor seq-counted here (the handler's first instruction takes this
     * seq), exactly as the CeDImu oracle jumps straight from the last poll
     * instruction to the handler. External and SCC68070 on-chip requests live
     * in separate pending masks, so the fast gate must cover both; otherwise a
     * timer interrupt can remain queued without reaching the delivery path. */
    if (recomp_pending_irq_mask()) recomp_take_irq();

    CdiTraceRecord *r = &s_trace_ring[s_trace_seq & CDI_TRACE_MASK];
    r->seq = s_trace_seq;
#ifdef CDI_COSIM
    /* MC-CDI-016 fault injection (COSIM-SPEC.md §5): fire BEFORE this seq's
     * state is captured below, so a hit at this exact seq is visible in the
     * very record it targets (registers AND the RAM hash) — never one seq
     * late. */
    cdi_cosim_maybe_inject(s_trace_seq);
#endif
    r->cpu = g_cpu;
    r->opcode = debug_peek_be16(g_cpu.PC);
    r->frame = (uint32_t)g_frame_count;
    r->total_cyc = g_total_cycles;
    r->a7_top = debug_peek_be32(g_cpu.A[7]);
#ifdef CDI_COSIM
    /* MC-CDI-016: incremental page-hash of both RAM banks (COSIM-SPEC.md
     * §3a) — O(pages dirtied since the last capture), kept off the hot path
     * when nothing changed. */
    r->ram0_h = cdi_cosim_ram_hash(0);
    r->ram1_h = cdi_cosim_ram_hash(1);
#else
    r->ram0_h = 0;
    r->ram1_h = 0;
#endif
    s_trace_seq++;
    if (atomic_exchange_explicit(&s_pause_requested, 0, memory_order_acq_rel))
        cdi_fault_hold();
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

/* ====================================================================== */
/*  IRQ-raise ring (every cdi_irq_raise: seq + PC + level)                 */
/* ====================================================================== */
/* Always-on log of device IRQ assertions so the RAISE boundary can be diffed
 * against the oracle's TAKE boundary (MC-CDI-007). Raises are rare (a handful
 * per second of boot), so a tiny ring + a remembered first-raise suffices. */
#define CDI_IRQ_RING_LEN 256u
typedef struct { uint64_t seq; uint32_t pc; uint8_t level; } CdiIrqRecord;
static CdiIrqRecord s_irq_ring[CDI_IRQ_RING_LEN];
static uint64_t     s_irq_count     = 0;
static uint64_t     s_irq_first_seq = 0;
static uint32_t     s_irq_first_pc  = 0;
static uint8_t      s_irq_first_lvl = 0;

void debug_record_irq_raise(uint8_t level) {
    if (s_irq_count == 0) {
        s_irq_first_seq = s_trace_seq;
        s_irq_first_pc  = g_cpu.PC;
        s_irq_first_lvl = level;
    }
    CdiIrqRecord *r = &s_irq_ring[s_irq_count % CDI_IRQ_RING_LEN];
    r->seq   = s_trace_seq;
    r->pc    = g_cpu.PC;
    r->level = level;
    s_irq_count++;
}

static void resp_irq_events(char *out, int outlen) {
    int n = snprintf(out, outlen,
        "{\"ok\":true,\"count\":%llu,\"first_seq\":%llu,\"first_pc\":%u,\"first_level\":%u,\"events\":[",
        (unsigned long long)s_irq_count, (unsigned long long)s_irq_first_seq,
        s_irq_first_pc, s_irq_first_lvl);
    uint64_t total  = s_irq_count;
    uint64_t oldest = total > CDI_IRQ_RING_LEN ? total - CDI_IRQ_RING_LEN : 0;
    int first = 1;
    for (uint64_t i = oldest; i < total && n < outlen - 80; i++) {
        const CdiIrqRecord *r = &s_irq_ring[i % CDI_IRQ_RING_LEN];
        n += snprintf(out + n, outlen - n, "%s{\"seq\":%llu,\"pc\":%u,\"level\":%u}",
                      first ? "" : ",", (unsigned long long)r->seq, r->pc, r->level);
        first = 0;
    }
    snprintf(out + n, outlen - n, "]}");
}

/* ====================================================================== */
/*  Interpreter-fallback classifier (always-on; per-PC + reason + region)  */
/* ====================================================================== */
/* A live aggregation keyed by PC (open-addressing hash) rather than a ring:
 * we want totals over the whole run, and distinct interpreted PCs are bounded
 * by code size (thousands), not by instruction count. Each slot accumulates the
 * interpreted-instruction count at that PC and an OR-mask of the reasons seen. */
#define CDI_FB_HASH_LEN  (1u << 16)            /* 65536 distinct PCs — ample for OS code */
#define CDI_FB_HASH_MASK (CDI_FB_HASH_LEN - 1u)

enum { FB_REG_VECTORS = 0, FB_REG_RAM0, FB_REG_RAM1, FB_REG_ROM, FB_REG_OTHER, FB_REGION_COUNT };

typedef struct {
    uint32_t pc;            /* interpreted PC (valid only when count > 0) */
    uint64_t count;         /* interpreted instructions executed at this PC */
    uint16_t reason_mask;   /* OR of (1u << reason) observed at this PC */
} CdiFbSlot;

static CdiFbSlot s_fb_hash[CDI_FB_HASH_LEN];
static uint64_t  s_fb_total = 0;                    /* total interpreted instructions */
static uint64_t  s_fb_by_reason[FB_REASON_COUNT];
static uint64_t  s_fb_by_region[FB_REGION_COUNT];
static uint32_t  s_fb_distinct = 0;
static uint64_t  s_fb_dropped  = 0;                 /* instrs lost when the table is full */

int g_fallback_reason = FB_NONE;

static int fb_region_of(uint32_t pc) {
    pc &= 0xFFFFFFu;
    if (pc < 0x000400u)                                           return FB_REG_VECTORS; /* 256 exception vectors */
    if (pc < CDI_RAM0_SIZE)                                       return FB_REG_RAM0;    /* 0x000400..0x07FFFF */
    if (pc >= CDI_RAM1_BASE && pc < CDI_RAM1_BASE + CDI_RAM1_SIZE) return FB_REG_RAM1;
    if (pc >= CDI_ROM_BASE  && pc < CDI_ROM_BASE  + CDI_ROM_SIZE)  return FB_REG_ROM;
    return FB_REG_OTHER;
}

void debug_trace_interp(uint32_t pc) {
    pc &= 0xFFFFFFu;
    int reason = g_fallback_reason;
    if (reason < 0 || reason >= FB_REASON_COUNT) reason = FB_NONE;
    s_fb_total++;
    s_fb_by_reason[reason]++;
    s_fb_by_region[fb_region_of(pc)]++;

    /* Open-addressing (linear-probe) insert/update keyed by pc. count==0 marks
     * an empty slot (PC 0 is the reset-SSP word, never executed as code). */
    uint32_t h = (pc * 2654435761u) & CDI_FB_HASH_MASK;
    for (uint32_t probe = 0; probe < CDI_FB_HASH_LEN; probe++) {
        CdiFbSlot *s = &s_fb_hash[(h + probe) & CDI_FB_HASH_MASK];
        if (s->count == 0) {                       /* empty: claim it */
            s->pc = pc;
            s->count = 1;
            s->reason_mask = (uint16_t)(1u << reason);
            s_fb_distinct++;
            return;
        }
        if (s->pc == pc) {                         /* existing PC: accumulate */
            s->count++;
            s->reason_mask |= (uint16_t)(1u << reason);
            return;
        }
    }
    s_fb_dropped++;                                /* table full (not expected for OS code) */
}

static void fb_reset(void) {
    memset(s_fb_hash, 0, sizeof s_fb_hash);
    memset(s_fb_by_reason, 0, sizeof s_fb_by_reason);
    memset(s_fb_by_region, 0, sizeof s_fb_by_region);
    s_fb_total = 0; s_fb_distinct = 0; s_fb_dropped = 0;
}

/* ---- Uncovered-entry set (trace-guided discovery seeds) ----
 * Distinct targets absent from both exact dispatch and the async native resume
 * map. A flat array with linear dedup is fine — this is per-dispatch, not
 * per-instruction. */
#define CDI_IT_MAX 16384
static uint32_t s_it_addr[CDI_IT_MAX];
static int      s_it_count = 0;

void debug_record_indirect_target(uint32_t addr) {
    addr &= 0xFFFFFFu;
    for (int i = 0; i < s_it_count; i++)
        if (s_it_addr[i] == addr) return;
    if (s_it_count < CDI_IT_MAX) s_it_addr[s_it_count++] = addr;
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
    while (*p && *p != '"' && i < buflen - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') buf[i++] = '\n';
            else if (*p == 'r') buf[i++] = '\r';
            else if (*p == 't') buf[i++] = '\t';
            else buf[i++] = *p;
            p++;
        } else {
            buf[i++] = *p++;
        }
    }
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
        "{\"ok\":true,\"insns\":%llu,\"blocks\":%llu,\"interp\":%llu,\"frame\":%llu,\"pc\":%u,"
        "\"halted\":%d,\"input\":%u,\"miss_count\":%u,\"miss_last\":%u,\"irq_pending\":%u,"
        "\"irq_raises\":%llu,\"irq_first_seq\":%llu}",
        (unsigned long long)g_native_insn_count, (unsigned long long)s_trace_seq,
        (unsigned long long)s_fb_total,
        (unsigned long long)g_frame_count, g_cpu.PC,
        g_halted, cdi_input_get(), g_miss_count_any, g_miss_last_addr, recomp_pending_irq_mask(),
        (unsigned long long)s_irq_count, (unsigned long long)s_irq_first_seq);
}

static void resp_pause(char *out, int outlen) {
    atomic_store_explicit(&s_pause_requested, 1, memory_order_release);
    snprintf(out, outlen,
             "{\"ok\":true,\"pause_requested\":true,\"seq\":%llu}",
             (unsigned long long)s_trace_seq);
}

static void resp_video_frame(char *out, int outlen) {
    uint16_t width, height;
    uint64_t generation;
    uint64_t hash = mcd212_framebuffer_hash(&width, &height, &generation);
    snprintf(out, outlen,
        "{\"ok\":true,\"width\":%u,\"height\":%u,\"generation\":%llu,"
        "\"argb_fnv1a\":\"%016llx\"}",
        width, height, (unsigned long long)generation,
        (unsigned long long)hash);
}

static void resp_audio_state(char *out, int outlen) {
    CdiAudioState state;
    cdi_audio_debug_state(&state);
    snprintf(out, outlen,
        "{\"ok\":true,\"rate\":%u,\"channels\":%u,\"sectors\":%llu,"
        "\"sample_frames\":%llu,\"queued_frames\":%u,"
        "\"dropped_frames\":%llu,\"pcm_fnv1a\":\"%016llx\"}",
        CDI_AUDIO_OUTPUT_RATE, CDI_AUDIO_OUTPUT_CHANNELS,
        (unsigned long long)state.sectors,
        (unsigned long long)state.sample_frames, state.queued_frames,
        (unsigned long long)state.dropped_frames,
        (unsigned long long)state.pcm_fnv1a);
}

/* Completed-frame history is the video-domain equivalent of the instruction
 * trace ring.  It lets a native/oracle framebuffer mismatch be localized to
 * its first field without repeatedly arming and rerunning either observer. */
static void resp_frame_hashes(const char *line, char *out, int outlen) {
    uint64_t from = 0, count = 64;
    int have_from = json_int(line, "from", &from);
    json_int(line, "count", &count);
    if (count > 512) count = 512;

    uint64_t total = s_frame_widx;
    uint64_t oldest = total > CDI_FRAME_RING_LEN ? total - CDI_FRAME_RING_LEN : 0;
    uint64_t start = have_from ? oldest : (total > count ? total - count : oldest);
    int n = snprintf(out, outlen, "{\"ok\":true,\"total\":%llu,\"records\":[",
                     (unsigned long long)total);
    uint64_t emitted = 0;
    for (uint64_t i = start; i < total && emitted < count && n < outlen - 128; i++) {
        CdiFrameRecord r = s_frame_ring[i % CDI_FRAME_RING_LEN];
        if (have_from && r.frame < from) continue;
        n += snprintf(out + n, outlen - n,
                      "%s{\"frame\":%llu,\"width\":%u,\"height\":%u,"
                      "\"argb_fnv1a\":\"%016llx\",\"mcd_count\":%u,"
                      "\"mcd_fnv1a\":\"%016llx\"}",
                      emitted ? "," : "", (unsigned long long)r.frame,
                      r.width, r.height, (unsigned long long)r.argb_hash,
                      r.mcd_count, (unsigned long long)r.mcd_hash);
        emitted++;
    }
    snprintf(out + n, outlen - n, "]}");
}

/* Read-only framebuffer inspection for UI-path coverage.  A full 768x560
 * ARGB frame does not fit in the line-oriented debug response buffer, so
 * expose one canonical scanline at a time.  The copy comes from the same
 * atomically published front buffer consumed by the SDL frontend; querying it
 * never advances or mutates the guest. */
static void resp_video_scanline(const char *line, char *out, int outlen) {
    static uint32_t frame[MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT];
    uint16_t width = 0, height = 0;
    uint64_t generation = 0, raw_y = 0;
    mcd212_framebuffer_info(&width, &height, &generation);
    if (!json_int(line, "y", &raw_y) || raw_y >= height) {
        snprintf(out, outlen,
                 "{\"ok\":false,\"error\":\"y out of range\",\"width\":%u,\"height\":%u}",
                 width, height);
        return;
    }
    mcd212_render_frame(frame);
    int n = snprintf(out, outlen,
                     "{\"ok\":true,\"width\":%u,\"height\":%u,"
                     "\"generation\":%llu,\"y\":%llu,\"argb\":\"",
                     width, height, (unsigned long long)generation,
                     (unsigned long long)raw_y);
    const uint32_t *row = frame + (size_t)raw_y * width;
    for (uint16_t x = 0; x < width && n < outlen - 12; x++)
        n += snprintf(out + n, outlen - n, "%08X", row[x]);
    snprintf(out + n, outlen - n, "\"}");
}

static void resp_video_state(char *out, int outlen) {
    uint16_t r[32], active;
    Mcd212VideoCursorState cursor;
    Mcd212VideoDebugState video;
    uint8_t csr1r, csr2r;
    uint32_t vline;
    mcd212_debug_state(r, &csr1r, &csr2r, &vline, &active);
    mcd212_video_debug_cursor(&cursor);
    mcd212_video_debug_state(&video);
    int n = snprintf(out, outlen,
        "{\"ok\":true,\"csr1r\":%u,\"csr2r\":%u,\"csr1w\":%u,"
        "\"csr2w\":%u,\"dcr1\":%u,\"dcr2\":%u,\"ddr1\":%u,"
        "\"ddr2\":%u,\"vsr1\":%u,\"vsr2\":%u,\"dcp1\":%u,"
        "\"dcp2\":%u,\"vline\":%u,\"active_line\":%u,"
        "\"cursor_x\":%u,\"cursor_y\":%u,\"cursor_enabled\":%u,"
        "\"cursor_color\":%u,\"cursor_double\":%u,"
        "\"cursor_blink_type\":%u,\"cursor_blink_on\":%u,"
        "\"cursor_blink_off\":%u,"
        "\"coding_a\":%u,\"coding_b\":%u,"
        "\"image_type_a\":%u,\"image_type_b\":%u,"
        "\"pixel_repeat_a\":%u,\"pixel_repeat_b\":%u,"
        "\"bpp_a\":%u,\"bpp_b\":%u,"
        "\"transparency_a\":%u,\"transparency_b\":%u,"
        "\"mix\":%u,\"plane_b_front\":%u,\"clut_bank\":%u,"
        "\"backdrop\":%u,\"clut_select_high\":%u,"
        "\"two_mattes\":%u,\"external_video\":%u,"
        "\"dyuv_a\":%u,\"dyuv_b\":%u,"
        "\"transparent_a\":%u,\"transparent_b\":%u,"
        "\"mask_a\":%u,\"mask_b\":%u,"
        "\"hold_enabled_a\":%u,\"hold_enabled_b\":%u,"
        "\"hold_factor_a\":%u,\"hold_factor_b\":%u,"
        "\"icf_a\":%u,\"icf_b\":%u,"
        "\"plane_line_first_a\":%u,\"plane_line_first_b\":%u,"
        "\"plane_line_nonblack_a\":%u,\"plane_line_nonblack_b\":%u,"
        "\"plane_line_fnv1a_a\":\"%016llx\","
        "\"plane_line_fnv1a_b\":\"%016llx\","
        "\"clut_fnv1a\":\"%016llx\","
        "\"cursor_pattern\":\"",
        csr1r, csr2r, r[0x10], r[0x00], r[0x12], r[0x02],
        r[0x18], r[0x08], r[0x14], r[0x04], r[0x1A], r[0x0A],
        vline, active, cursor.x, cursor.y, cursor.enabled, cursor.color,
        cursor.double_resolution, cursor.blink_type, cursor.blink_on,
        cursor.blink_off, video.coding[0], video.coding[1],
        video.image_type[0], video.image_type[1],
        video.pixel_repeat[0], video.pixel_repeat[1],
        video.bpp[0], video.bpp[1],
        video.transparency[0], video.transparency[1], video.mix,
        video.plane_b_front, video.clut_bank, video.backdrop,
        video.clut_select_high, video.two_mattes, video.external_video,
        video.dyuv_start[0], video.dyuv_start[1],
        video.transparent_color[0], video.transparent_color[1],
        video.mask_color[0], video.mask_color[1],
        video.hold_enabled[0], video.hold_enabled[1],
        video.hold_factor[0], video.hold_factor[1],
        video.icf[0], video.icf[1],
        video.plane_line_first[0], video.plane_line_first[1],
        video.plane_line_nonblack[0], video.plane_line_nonblack[1],
        (unsigned long long)video.plane_line_hash[0],
        (unsigned long long)video.plane_line_hash[1],
        (unsigned long long)video.clut_hash);
    for (int i = 0; i < 16 && n < outlen - 8; i++)
        n += snprintf(out + n, outlen - n, "%04X", cursor.pattern[i]);
    snprintf(out + n, outlen - n, "\"}");
}

static void resp_video_clut(char *out, int outlen) {
    uint32_t clut[256];
    int n;
    mcd212_video_debug_clut(clut);
    n = snprintf(out, outlen, "{\"ok\":true,\"colors\":[");
    for (int i = 0; i < 256 && n < outlen - 24; i++)
        n += snprintf(out + n, outlen - n, "%s%u", i ? "," : "", clut[i]);
    snprintf(out + n, outlen - n, "]}");
}

/* set_input: permanent deterministic controller surface. `mask` uses the
 * CDI_INPUT_* ABI; named fields may be supplied instead for human clients.
 * This only publishes host state. The CPU thread generates a real timed IKAT
 * channel-A packet and IRQ, so tests exercise the guest input driver. */
static void resp_set_input(const char *line, char *out, int outlen) {
    uint64_t raw = 0, value = 0, raw_x = 0, raw_y = 0;
    uint32_t mask = 0;
    int pending_x, pending_y;
    if (json_int(line, "mask", &raw)) {
        mask = (uint32_t)raw;
    } else {
        if (json_int(line, "left",  &value) && value) mask |= CDI_INPUT_LEFT;
        if (json_int(line, "up",    &value) && value) mask |= CDI_INPUT_UP;
        if (json_int(line, "right", &value) && value) mask |= CDI_INPUT_RIGHT;
        if (json_int(line, "down",  &value) && value) mask |= CDI_INPUT_DOWN;
        if (json_int(line, "btn1",  &value) && value) mask |= CDI_INPUT_BTN1;
        if (json_int(line, "btn2",  &value) && value) mask |= CDI_INPUT_BTN2;
    }
    cdi_input_set(mask);
    {
        int have_x = json_int(line, "dx", &raw_x);
        int have_y = json_int(line, "dy", &raw_y);
        if (have_x || have_y)
            cdi_input_add_relative((int)(int64_t)raw_x,
                                   (int)(int64_t)raw_y);
    }
    cdi_input_pending_relative(&pending_x, &pending_y);
    snprintf(out, outlen,
             "{\"ok\":true,\"input\":%u,\"pending_dx\":%d,\"pending_dy\":%d}",
             cdi_input_get(), pending_x, pending_y);
}

static void resp_ikat_state(char *out, int outlen) {
    uint8_t regs[15], remaining[4];
    double pointer_ns;
    int packets;
    slave_debug_state(regs, remaining, &pointer_ns, &packets);
    int n = snprintf(out, outlen,
        "{\"ok\":true,\"input\":%u,\"pointer_ns\":%.0f,\"cursor_packets\":%d,"
        "\"out_remaining\":[%u,%u,%u,%u],\"regs\":[",
        cdi_input_get(), pointer_ns, packets,
        remaining[0], remaining[1], remaining[2], remaining[3]);
    for (int i = 0; i < 15 && n < outlen - 16; i++)
        n += snprintf(out + n, outlen - n, "%s%u", i ? "," : "", regs[i]);
    snprintf(out + n, outlen - n, "]}");
}

static void resp_ikat_events(char *out, int outlen) {
    CdiIkatEvent events[64];
    uint64_t total;
    int count = slave_debug_events(events, 64, &total);
    int n = snprintf(out, outlen, "{\"ok\":true,\"total\":%llu,\"events\":[",
                     (unsigned long long)total);
    for (int i = 0; i < count && n < outlen - 128; i++) {
        CdiIkatEvent *e = &events[i];
        n += snprintf(out + n, outlen - n,
            "%s{\"seq\":%llu,\"trace_seq\":%llu,\"pc\":%u,"
            "\"frame\":%llu,\"cycles\":%llu,\"type\":%u,"
            "\"channel\":%u,\"data\":\"",
            i ? "," : "", (unsigned long long)e->seq,
            (unsigned long long)e->trace_seq, e->pc,
            (unsigned long long)e->frame, (unsigned long long)e->cycles,
            e->type, e->channel);
        for (unsigned j = 0; j < e->length && n < outlen - 8; j++)
            n += snprintf(out + n, outlen - n, "%02X", e->data[j]);
        n += snprintf(out + n, outlen - n, "\"}");
    }
    snprintf(out + n, outlen - n, "]}");
}

static void resp_ciap_events(const char *line, char *out, int outlen) {
    uint64_t from = UINT64_MAX, count = 128;
    uint64_t filter_offset = UINT64_MAX, frame_min = 0;
    json_int(line, "from", &from);
    json_int(line, "count", &count);
    json_int(line, "offset", &filter_offset);
    json_int(line, "frame_min", &frame_min);
    if (count > 256) count = 256;

    CdiCiapEvent events[256];
    uint64_t total = 0, oldest = 0;
    int got;
    if (filter_offset == UINT64_MAX && frame_min == 0) {
        got = cdic_debug_events(events, (int)count, from, &total, &oldest);
    } else {
        CdiCiapEvent page[256];
        uint64_t cursor;
        int page_count;
        got = 0;
        cdic_debug_events(page, 0, UINT64_MAX, &total, &oldest);
        cursor = from == UINT64_MAX ? oldest : from;
        if (cursor < oldest) cursor = oldest;
        while (cursor < total && got < (int)count) {
            page_count = cdic_debug_events(page, 256, cursor, NULL, NULL);
            if (!page_count) break;
            for (int i = 0; i < page_count && got < (int)count; i++) {
                if ((filter_offset == UINT64_MAX ||
                     page[i].offset == filter_offset) &&
                    page[i].frame >= frame_min)
                    events[got++] = page[i];
            }
            cursor = page[page_count - 1].seq + 1;
        }
    }
    int n = snprintf(out, outlen,
        "{\"ok\":true,\"total\":%llu,\"oldest\":%llu,\"events\":[",
        (unsigned long long)total, (unsigned long long)oldest);
    for (int i = 0; i < got && n < outlen - 192; i++) {
        const CdiCiapEvent *e = &events[i];
        n += snprintf(out + n, outlen - n,
            "%s{\"seq\":%llu,\"trace_seq\":%llu,\"pc\":%u,"
            "\"frame\":%llu,\"cycles\":%llu,\"offset\":%u,"
            "\"size\":%u,\"write\":%u,\"value\":%u}",
            i ? "," : "", (unsigned long long)e->seq,
            (unsigned long long)e->trace_seq, e->pc,
            (unsigned long long)e->frame, (unsigned long long)e->cycles,
            e->offset, e->size, e->write, e->value);
    }
    snprintf(out + n, outlen - n, "]}");
}

static void resp_ciap_state(char *out, int outlen) {
    uint32_t drive_lba, last_lba;
    uint8_t file, channel, submode, coding;
    int selected, running, waiting_ack;
    cdic_debug_state(&drive_lba, &last_lba, &file, &channel, &submode,
                     &coding, &selected, &running, &waiting_ack);
    snprintf(out, outlen,
        "{\"ok\":true,\"drive_lba\":%u,\"last_lba\":%u,"
        "\"file\":%u,\"channel\":%u,\"submode\":%u,"
        "\"coding\":%u,\"selected\":%d,\"running\":%d,"
        "\"waiting_ack\":%d}",
        drive_lba, last_lba, file, channel, submode, coding, selected,
        running, waiting_ack);
}

static void resp_disc_state(char *out, int outlen) {
    snprintf(out, outlen,
        "{\"ok\":true,\"present\":%d,\"sectors\":%u,\"track_mode\":%d}",
        cdi_media_present(), cdi_media_sector_count(), cdi_media_track_mode());
}

static void resp_mount_disc(const char *line, char *out, int outlen) {
    char path[1024];
    if (!json_str(line, "path", path, sizeof path)) {
        snprintf(out, outlen, "{\"ok\":false,\"error\":\"missing path\"}");
        return;
    }
    if (!cdi_media_mount(path)) {
        snprintf(out, outlen, "{\"ok\":false,\"error\":\"disc mount failed\"}");
        return;
    }
    resp_disc_state(out, outlen);
}

static void resp_eject_disc(char *out, int outlen) {
    cdi_media_eject();
    resp_disc_state(out, outlen);
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
            "%s{\"seq\":%llu,\"pc\":%u,\"op\":%u,\"sr\":%u", i ? "," : "",
            (unsigned long long)recs[i].seq, c->PC, recs[i].opcode, c->SR);
        for (int r = 0; r < 8; r++) n += snprintf(out + n, outlen - n, ",\"d%d\":%u", r, c->D[r]);
        for (int r = 0; r < 8; r++) n += snprintf(out + n, outlen - n, ",\"a%d\":%u", r, c->A[r]);
        n += snprintf(out + n, outlen - n, ",\"a7top\":%u,\"frame\":%u,\"tcyc\":%llu",
                      recs[i].a7_top, recs[i].frame, (unsigned long long)recs[i].total_cyc);
        /* MC-CDI-016 (COSIM-SPEC.md §2a/§4): usp/ssp must be emitted in the
         * CANONICAL form (matching the oracle, which is always canonical via
         * its A7-alias). The native M68KState stores the INACTIVE shadow in
         * .USP/.SSP — the ACTIVE role's live value is in .A[7] and the
         * matching shadow field is stale. Raw-emitting .USP/.SSP would
         * false-diverge against the oracle throughout supervisor mode.
         * ram0_h/ram1_h are 64-bit hashes emitted as lowercase 16-char hex
         * STRINGS (not JSON numbers) to avoid float precision loss; the
         * coordinator parses them with int(x, 16). */
        {
            uint32_t _s = c->SR & 0x2000u;   /* S (supervisor) bit */
            uint32_t emit_ssp = _s ? c->A[7] : c->SSP;
            uint32_t emit_usp = _s ? c->USP  : c->A[7];
            n += snprintf(out + n, outlen - n,
                          ",\"usp\":%u,\"ssp\":%u,\"ram0_h\":\"%016llx\",\"ram1_h\":\"%016llx\"",
                          emit_usp, emit_ssp,
                          (unsigned long long)recs[i].ram0_h, (unsigned long long)recs[i].ram1_h);
        }
        n += snprintf(out + n, outlen - n, "}");
    }
    snprintf(out + n, outlen - n, "]}");
}

/* Compact MC-CDI-009 view of the same always-on ring. `tcyc` is sampled at
 * instruction entry, so record N costs tcyc[N+1]-tcyc[N]. */
static void resp_cycle_trace(const char *line, char *out, int outlen) {
    uint64_t count = 16, from = 0;
    json_int(line, "count", &count);
    if (count > 512) count = 512;
    static CdiTraceRecord recs[512];
    int got;
    uint64_t first = 0;
    int have_from = json_int(line, "from", &from);
    if (have_from) got = trace_range(recs, from, (int)count, &first);
    else           got = trace_tail(recs, (int)count);
    int n = snprintf(out, outlen, "{\"ok\":true,\"total\":%llu,\"records\":[",
                     (unsigned long long)s_trace_seq);
    for (int i = 0; i < got && n < outlen - 128; i++) {
        n += snprintf(out + n, outlen - n,
                      "%s{\"seq\":%llu,\"pc\":%u,\"op\":%u,\"cycle_seam\":0,\"tcyc\":%llu}",
                      i ? "," : "", (unsigned long long)recs[i].seq,
                      recs[i].cpu.PC, recs[i].opcode,
                      (unsigned long long)recs[i].total_cyc);
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

/* interp_report: classify the hybrid-interpreter fallback. Params:
 *   count  (optional) top PCs by instruction count to return (default 40, max 200)
 *   reason (optional) restrict the top-PC list to PCs that saw this reason index
 *   reset  (optional) zero ALL counters AFTER building the response (window scoping)
 * by_reason / by_region are arrays indexed by the FB_* / FB_REG_* enums (the
 * client labels them). `native` is g_native_insn_count for the recompiled-vs-
 * interpreted ratio. Always-on aggregate; reads are racy but monotonic. */
static void resp_interp_report(const char *line, char *out, int outlen) {
    uint64_t count = 40, reason_filter = 0, reset = 0;
    json_int(line, "count", &count);
    int have_reason = json_int(line, "reason", &reason_filter);
    json_int(line, "reset", &reset);
    if (count > 200) count = 200;

    /* Top-K by count: insertion into a descending buffer (O(distinct * cap)). */
    static CdiFbSlot top[200];
    int cap = (int)count, have = 0;
    for (uint32_t i = 0; i < CDI_FB_HASH_LEN; i++) {
        const CdiFbSlot *s = &s_fb_hash[i];
        if (s->count == 0) continue;
        if (have_reason && !(s->reason_mask & (1u << (uint32_t)reason_filter))) continue;
        if (have < cap) {
            int j = have++;
            while (j > 0 && top[j - 1].count < s->count) { top[j] = top[j - 1]; j--; }
            top[j] = *s;
        } else if (cap > 0 && s->count > top[have - 1].count) {
            int j = have - 1;
            while (j > 0 && top[j - 1].count < s->count) { top[j] = top[j - 1]; j--; }
            top[j] = *s;
        }
    }

    int n = snprintf(out, outlen,
        "{\"ok\":true,\"total\":%llu,\"native\":%llu,\"distinct\":%u,\"dropped\":%llu,"
        "\"by_reason\":[",
        (unsigned long long)s_fb_total, (unsigned long long)g_native_insn_count,
        s_fb_distinct, (unsigned long long)s_fb_dropped);
    for (int r = 0; r < FB_REASON_COUNT && n < outlen; r++)
        n += snprintf(out + n, outlen - n, "%s%llu", r ? "," : "",
                      (unsigned long long)s_fb_by_reason[r]);
    n += snprintf(out + n, outlen - n, "],\"by_region\":[");
    for (int r = 0; r < FB_REGION_COUNT && n < outlen; r++)
        n += snprintf(out + n, outlen - n, "%s%llu", r ? "," : "",
                      (unsigned long long)s_fb_by_region[r]);
    n += snprintf(out + n, outlen - n, "],\"top\":[");
    for (int k = 0; k < have && n < outlen - 96; k++)
        n += snprintf(out + n, outlen - n,
            "%s{\"pc\":%u,\"count\":%llu,\"reasons\":%u,\"region\":%d}",
            k ? "," : "", top[k].pc, (unsigned long long)top[k].count,
            top[k].reason_mask, fb_region_of(top[k].pc));
    snprintf(out + n, outlen - n, "]}");

    if (reset) fb_reset();
}

/* indirect_targets: distinct genuinely uncovered control-flow entries. Emitted
 * unsorted; the collector sorts + filters in-ROM. Capped by the 64K response
 * buffer (count is small). */
static void resp_indirect_targets(char *out, int outlen) {
    int n = snprintf(out, outlen, "{\"ok\":true,\"count\":%d,\"targets\":[", s_it_count);
    for (int i = 0; i < s_it_count && n < outlen - 16; i++)
        n += snprintf(out + n, outlen - n, "%s%u", i ? "," : "", s_it_addr[i]);
    snprintf(out + n, outlen - n, "]}");
}

#ifdef CDI_COSIM
/* cosim_full_ram_hash (MC-CDI-016, COSIM-SPEC.md §5): recompute BOTH bank
 * hashes from scratch over live RAM — NOT the incremental page_hash array —
 * and return them alongside the INCREMENTAL hash (cdi_cosim_ram_hash, dirty-
 * page refresh + fold) taken at the SAME instant. Gate 4 compares ram*_h vs
 * ram*_h_inc from this one response, which is immune to the one-instruction
 * skew between the parked machine state (pre-seq-N) and the last captured
 * ring record (pre-seq-(N-1)); a mismatch = a missed RAM-write-hook site
 * (COSIM-SPEC.md §3b). */
static void resp_cosim_full_ram_hash(char *out, int outlen) {
    uint64_t r0 = cdi_cosim_full_ram_hash(0);
    uint64_t r1 = cdi_cosim_full_ram_hash(1);
    uint64_t r0i = cdi_cosim_ram_hash(0);
    uint64_t r1i = cdi_cosim_ram_hash(1);
    snprintf(out, outlen,
        "{\"ok\":true,\"seq\":%llu,\"ram0_h\":\"%016llx\",\"ram1_h\":\"%016llx\","
        "\"ram0_h_inc\":\"%016llx\",\"ram1_h_inc\":\"%016llx\"}",
        (unsigned long long)s_trace_seq, (unsigned long long)r0, (unsigned long long)r1,
        (unsigned long long)r0i, (unsigned long long)r1i);
}
#endif

/* Returns 1 to keep the connection open, 0 to close it (quit). */
static int handle_line(const char *line, char *out, int outlen) {
    char cmd[32];
    parse_cmd(line, cmd, sizeof cmd);

    if (!strcmp(cmd, "ping"))               snprintf(out, outlen, "{\"ok\":true,\"pong\":true,\"session\":\"%s\"}", s_cosim_session);
    else if (!strcmp(cmd, "status"))        resp_status(out, outlen);
    else if (!strcmp(cmd, "pause"))         resp_pause(out, outlen);
    else if (!strcmp(cmd, "set_input"))     resp_set_input(line, out, outlen);
    else if (!strcmp(cmd, "emu_ikat_state")) resp_ikat_state(out, outlen);
    else if (!strcmp(cmd, "ikat_events"))    resp_ikat_events(out, outlen);
    else if (!strcmp(cmd, "ciap_events"))    resp_ciap_events(line, out, outlen);
    else if (!strcmp(cmd, "ciap_state"))     resp_ciap_state(out, outlen);
    else if (!strcmp(cmd, "video_frame"))    resp_video_frame(out, outlen);
    else if (!strcmp(cmd, "audio_state"))    resp_audio_state(out, outlen);
    else if (!strcmp(cmd, "frame_hashes"))   resp_frame_hashes(line, out, outlen);
    else if (!strcmp(cmd, "video_scanline")) resp_video_scanline(line, out, outlen);
    else if (!strcmp(cmd, "video_state"))    resp_video_state(out, outlen);
    else if (!strcmp(cmd, "video_clut"))     resp_video_clut(out, outlen);
    else if (!strcmp(cmd, "disc_state"))     resp_disc_state(out, outlen);
    else if (!strcmp(cmd, "mount_disc"))     resp_mount_disc(line, out, outlen);
    else if (!strcmp(cmd, "eject_disc"))     resp_eject_disc(out, outlen);
    else if (!strcmp(cmd, "irq_events"))    resp_irq_events(out, outlen);
    else if (!strcmp(cmd, "get_registers")) resp_registers(out, outlen);
    else if (!strcmp(cmd, "read_mem"))      resp_read_mem(line, out, outlen);
    else if (!strcmp(cmd, "trace"))         resp_trace(line, out, outlen);
    else if (!strcmp(cmd, "cycle_trace"))   resp_cycle_trace(line, out, outlen);
    else if (!strcmp(cmd, "stores"))        resp_stores(line, out, outlen);
    else if (!strcmp(cmd, "interp_report")) resp_interp_report(line, out, outlen);
    else if (!strcmp(cmd, "indirect_targets")) resp_indirect_targets(out, outlen);
    else if (!strcmp(cmd, "dispatch_miss_info")) resp_miss(out, outlen);
#ifdef CDI_COSIM
    else if (!strcmp(cmd, "cosim_full_ram_hash")) resp_cosim_full_ram_hash(out, outlen);
#endif
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
                    "(ping/status/pause/set_input/emu_ikat_state/ikat_events/video_frame/video_scanline/video_state/video_clut/"
                    "ciap_events/disc_state/mount_disc/eject_disc/get_registers/read_mem/"
                    "trace/cycle_trace/stores/interp_report/"
                    "dispatch_miss_info"
#ifdef CDI_COSIM
                    "/cosim_full_ram_hash"
#endif
                    ")\n", s_port);
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
