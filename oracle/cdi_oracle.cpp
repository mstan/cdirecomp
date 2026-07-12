/*
 * cdi_oracle.cpp — headless CeDImu driver exposing the CdiRuntime debug surface.
 *
 * CeDImu is the behavioral oracle (PRINCIPLES.md ground truth). This links the
 * wxWidgets-free CeDImu core (external/CeDImu/src/CDI), boots the same system
 * ROM CdiRuntime boots, single-steps the SCC68070 capturing per-instruction
 * {entry PC, register file} into a ring, and serves the SAME JSON-line TCP
 * protocol as runner/src/debug_server.c — on port 4381 (native +1). That makes
 * native-vs-oracle first-divergence diffing symmetric: tools/first_divergence.py
 * pages both traces from seq 0 and reports the first PC mismatch.
 *
 * The native runtime captures one trace entry per instruction (the per-block
 * glue_check_vblank hook fires once per instruction); CeDImu steps one
 * instruction per Run(false). So the two PC streams are index-alignable.
 */
#include "CDI.hpp"
#include "CDIConfig.hpp"
#include "common/types.hpp"          /* Boards */
#include "common/Callbacks.hpp"
#include "cores/SCC68070/SCC68070.hpp"
#include "cosim_state.hpp"           /* MC-CDI-016 differential co-sim: RAM hash + fault injection, see docs/COSIM-SPEC.md */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define BAD_SOCK INVALID_SOCKET
  #define close_sock closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  typedef int sock_t;
  #define BAD_SOCK (-1)
  #define close_sock close
#endif

using Reg = SCC68070::Register;

/* ---- block-trace ring (mirrors the native CdiTraceRecord shape) ---- */
struct TraceRec {
    uint64_t seq; uint32_t pc, sr, d[8], a[8], a7top, frame; uint64_t tcyc;
    /* MC-CDI-016 (COSIM-SPEC.md §2a/§2b): the two inactive-stack-pointer
     * shadows + the two incremental RAM bank hashes, added alongside the
     * native side so "same registers, different memory" and inactive-SP
     * divergences stop being invisible to seq-aligned diffing. */
    uint32_t usp, ssp;
    uint64_t ram0_h, ram1_h;
};
static constexpr uint32_t RING = 1u << 20;   /* 1048576 — match the native trace ring
                                                so [1,~1M) is diffable from seq 1 (boot
                                                reaches the IRQ frontier at ~seq 575k) */
static constexpr uint32_t MASK = RING - 1;
static std::vector<TraceRec> g_ring(RING);
static uint64_t g_seq = 0;
static CDI *g_cdi = nullptr;

/* ---- MC-CDI-016 fault injection (COSIM-SPEC.md §5): --cosim-inject /
 * CDI_COSIM_INJECT, applied once in capture() when g_seq reaches the target
 * (free-run doctrine: a startup knob, never a mid-run command). ---- */
#ifdef CDI_COSIM
static bool g_inject_armed = false, g_inject_done = false, g_inject_is_ram = false;
static uint64_t g_inject_seq = 0;
static uint32_t g_inject_idx = 0, g_inject_xor = 0;
/* Pre-Run RAM bank hashes, snapshotted in the step loop BEFORE Run(false) so
 * they reflect the same pre-instruction instant as the `pre` register map and
 * the native ring's instruction-entry capture (COSIM-SPEC.md §1/§2 alignment).
 * Computing them inside capture() (post-Run) would make ram*_h reflect the
 * instruction's own writes while the registers reflect entry state — an
 * off-by-one that false-diverges at every RAM-writing instruction. */
static uint64_t g_pre_ram0 = 0, g_pre_ram1 = 0;

/* Parse "<seq>:<kind>:<idx>:<xorhex>", kind = "ram"|"reg". seq/idx accept any
 * base strtoull recognises (0x... or decimal); xor is always hex (no 0x
 * needed) per the field name in COSIM-SPEC.md §5. Returns false (injection
 * left disarmed) on any malformed spec. */
static bool parse_cosim_inject(const char *spec) {
    if (!spec || !*spec) return false;
    char kind[8] = {0};
    char *end;
    unsigned long long seq = strtoull(spec, &end, 0);
    if (end == spec || *end != ':') return false;
    const char *p = end + 1;
    int k = 0; while (*p && *p != ':' && k < (int)sizeof(kind) - 1) kind[k++] = *p++;
    kind[k] = 0;
    if (*p != ':') return false;
    p++;
    unsigned long long idx = strtoull(p, &end, 0);
    if (end == p || *end != ':') return false;
    p = end + 1;
    unsigned long long xorv = strtoull(p, &end, 16);
    if (end == p) return false;
    g_inject_seq = seq; g_inject_idx = (uint32_t)idx; g_inject_xor = (uint32_t)xorv;
    g_inject_is_ram = !strcmp(kind, "ram");
    g_inject_armed = true; g_inject_done = false;
    return true;
}
#endif

/* Side-effect-free byte/longword peek of guest memory (definition below). Used
 * to sample [A7] (stack top) per record — mirrors the native a7_top capture so
 * the realign tool can fold the stack top into its alignment key. */
static int peek_byte(uint32_t addr, uint8_t *out);
static uint32_t peek_be32(uint32_t addr) {
    uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    peek_byte(addr,     &b0); peek_byte(addr + 1, &b1);
    peek_byte(addr + 2, &b2); peek_byte(addr + 3, &b3);
    return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
}

/* Record one executed instruction: `pc` is currentPC (the address that just
 * executed) and `r` is the register snapshot taken BEFORE the step (entry state
 * of that instruction). This matches the native ring exactly: the generator taps
 * the trace at instruction ENTRY (PC just set, registers = state before the
 * instruction's effects). So both sides store {instruction PC, entry-state regs}
 * in program order. (Seq 0's registers are the pre-reset defaults — a one-
 * instruction seam from CeDImu bundling reset into the first step; --start 1.) */
static void capture(uint32_t pc, std::map<Reg, uint32_t> r) {
    /* Fault injection and the RAM-hash snapshot are done in the step loop at
     * the pre-Run instant (see g_pre_ram0/1), NOT here — capture() runs after
     * Run(false), so recomputing ram*_h here would reflect this instruction's
     * own writes (off-by-one vs the entry-state registers). */
    TraceRec &t = g_ring[g_seq & MASK];
    t.seq = g_seq;
    t.pc = pc;
    t.sr = r.at(Reg::SR);
    for (int i = 0; i < 8; i++) t.d[i] = r.at(static_cast<Reg>(static_cast<int>(Reg::D0) + i));
    for (int i = 0; i < 8; i++) t.a[i] = r.at(static_cast<Reg>(static_cast<int>(Reg::A0) + i));
    t.a7top = peek_be32(t.a[7]);   /* stack top at instruction entry */
    t.frame = g_cdi->GetTotalFrameCount();   /* MCD212 m_totalFrameCount — diff vs native g_frame_count per seq */
    t.tcyc  = g_cdi->m_cpu.totalCycleCount;   /* SCC68070 clock — diff vs native g_total_cycles per seq */
    /* COSIM-SPEC.md §2a: inactive-SP shadows, unconditional (cheap, always
     * present — like native's c->USP/c->SSP straight off the CPU struct). */
    t.usp = r.at(Reg::USP);
    t.ssp = r.at(Reg::SSP);
#ifdef CDI_COSIM
    /* COSIM-SPEC.md §2b/§3a: pre-instruction RAM bank hashes, snapshotted in
     * the step loop before Run(false) so they align with the entry-state
     * registers (NOT recomputed here post-Run — see g_pre_ram0/1). */
    t.ram0_h = g_pre_ram0;
    t.ram1_h = g_pre_ram1;
#else
    t.ram0_h = 0;
    t.ram1_h = 0;
#endif
    g_seq++;
}

/* ---- tiny JSON-ish request parsing (matches debug_server.c) ---- */
static bool json_int(const char *s, const char *key, uint64_t *out) {
    char pat[32]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(s, pat); if (!p) return false;
    p = strchr(p + strlen(pat), ':'); if (!p) return false;
    *out = strtoull(p + 1, nullptr, 0); return true;
}
static void parse_cmd(const char *line, char *cmd, int len) {
    const char *p = strstr(line, "\"cmd\"");
    if (p && (p = strchr(p + 5, ':')) && (p = strchr(p, '"'))) {
        p++; int i = 0; while (*p && *p != '"' && i < len - 1) cmd[i++] = *p++; cmd[i] = 0; return;
    }
    int i = 0; p = line; while (*p == ' ') p++;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < len - 1) cmd[i++] = *p++;
    cmd[i] = 0;
}

static int peek_byte(uint32_t addr, uint8_t *out) {
    try {
        const uint8_t *p = g_cdi->GetPointer(addr);
        if (p) { *out = *p; return 1; }
    } catch (...) {}
    *out = 0; return 0;
}

static void trace_records(char *out, int outlen, uint64_t from, int count, bool have_from) {
    static TraceRec recs[256];
    if (count > 256) count = 256;
    int got = 0;
    uint64_t total = g_seq;
    uint64_t oldest = total > RING ? total - RING : 0;
    if (have_from) {
        if (from < oldest) from = oldest;
        for (uint64_t s = from; s < total && got < count; s++, got++) recs[got] = g_ring[s & MASK];
    } else {
        uint64_t n = (uint64_t)count > total ? total : (uint64_t)count;
        uint64_t start = total - n;
        for (uint64_t i = 0; i < n; i++, got++) recs[got] = g_ring[(start + i) & MASK];
    }
    int n = snprintf(out, outlen, "{\"ok\":true,\"total\":%llu,\"records\":[", (unsigned long long)g_seq);
    for (int i = 0; i < got && n < outlen - 400; i++) {
        n += snprintf(out + n, outlen - n, "%s{\"seq\":%llu,\"pc\":%u,\"sr\":%u",
                      i ? "," : "", (unsigned long long)recs[i].seq, recs[i].pc, recs[i].sr);
        for (int r = 0; r < 8; r++) n += snprintf(out + n, outlen - n, ",\"d%d\":%u", r, recs[i].d[r]);
        for (int r = 0; r < 8; r++) n += snprintf(out + n, outlen - n, ",\"a%d\":%u", r, recs[i].a[r]);
        n += snprintf(out + n, outlen - n, ",\"a7top\":%u,\"frame\":%u,\"tcyc\":%llu",
                      recs[i].a7top, recs[i].frame, (unsigned long long)recs[i].tcyc);
        /* COSIM-SPEC.md §4: usp/ssp as uint32, ram0_h/ram1_h as lowercase
         * 16-char hex strings (not JSON numbers — avoids float precision
         * loss on a 64-bit value). */
        n += snprintf(out + n, outlen - n, ",\"usp\":%u,\"ssp\":%u,\"ram0_h\":\"%016llx\",\"ram1_h\":\"%016llx\"",
                      recs[i].usp, recs[i].ssp,
                      (unsigned long long)recs[i].ram0_h, (unsigned long long)recs[i].ram1_h);
        n += snprintf(out + n, outlen - n, "}");
    }
    snprintf(out + n, outlen - n, "]}");
}

static int handle_line(const char *line, char *out, int outlen) {
    char cmd[32]; parse_cmd(line, cmd, sizeof cmd);
    auto regs = g_cdi->m_cpu.GetCPURegisters();
    if (!strcmp(cmd, "ping")) snprintf(out, outlen, "{\"ok\":true,\"pong\":true}");
    else if (!strcmp(cmd, "status"))
        snprintf(out, outlen, "{\"ok\":true,\"insns\":%llu,\"blocks\":%llu,\"frame\":0,\"pc\":%u,"
                 "\"miss_count\":0,\"miss_last\":0,\"irq_pending\":0,\"oracle\":true}",
                 (unsigned long long)g_seq, (unsigned long long)g_seq, regs.at(Reg::PC));
    else if (!strcmp(cmd, "get_registers")) {
        int n = snprintf(out, outlen, "{\"ok\":true,\"pc\":%u,\"sr\":%u,\"usp\":%u,\"ssp\":%u",
                         regs.at(Reg::PC), regs.at(Reg::SR), regs.at(Reg::USP), regs.at(Reg::SSP));
        for (int i = 0; i < 8; i++) n += snprintf(out + n, outlen - n, ",\"d%d\":%u", i, regs.at(static_cast<Reg>(static_cast<int>(Reg::D0) + i)));
        for (int i = 0; i < 8; i++) n += snprintf(out + n, outlen - n, ",\"a%d\":%u", i, regs.at(static_cast<Reg>(static_cast<int>(Reg::A0) + i)));
        snprintf(out + n, outlen - n, "}");
    }
#ifdef CDI_COSIM
    else if (!strcmp(cmd, "cosim_full_ram_hash")) {
        /* COSIM-SPEC.md §5: full from-scratch recompute over live RAM (NOT
         * the incremental page_hash cache) — Gate 4 compares this against
         * the incremental ram0_h_inc/ram1_h_inc taken AT THE SAME INSTANT
         * (same response, same live state) so the comparison is immune to
         * the one-instruction skew between the parked machine state and the
         * last captured ring record. A mismatch means a missed write-hook
         * site (§3b). cdi_cosim_ram_hash() only refreshes dirty pages, so
         * calling it here does not disturb the incremental cache the step
         * loop relies on — it just folds in any pages already marked dirty. */
        uint64_t r0 = cdi_cosim_full_ram_hash(0);
        uint64_t r1 = cdi_cosim_full_ram_hash(1);
        uint64_t r0i = cdi_cosim_ram_hash(0);
        uint64_t r1i = cdi_cosim_ram_hash(1);
        snprintf(out, outlen, "{\"ok\":true,\"seq\":%llu,"
                 "\"ram0_h\":\"%016llx\",\"ram1_h\":\"%016llx\","
                 "\"ram0_h_inc\":\"%016llx\",\"ram1_h_inc\":\"%016llx\"}",
                 (unsigned long long)g_seq,
                 (unsigned long long)r0, (unsigned long long)r1,
                 (unsigned long long)r0i, (unsigned long long)r1i);
    }
#endif
    else if (!strcmp(cmd, "read_mem")) {
        uint64_t addr = 0, len = 16;
        if (!json_int(line, "addr", &addr)) { snprintf(out, outlen, "{\"ok\":false,\"error\":\"addr required\"}"); return 1; }
        json_int(line, "len", &len); if (len > 4096) len = 4096;
        int n = snprintf(out, outlen, "{\"ok\":true,\"addr\":%u,\"bytes\":\"", (uint32_t)addr);
        for (uint64_t i = 0; i < len && n < outlen - 4; i++) {
            uint8_t b; if (peek_byte((uint32_t)(addr + i), &b)) n += snprintf(out + n, outlen - n, "%02X", b);
            else n += snprintf(out + n, outlen - n, "--");
        }
        snprintf(out + n, outlen - n, "\"}");
    }
    else if (!strcmp(cmd, "trace")) {
        uint64_t from = 0, count = 16; json_int(line, "count", &count);
        bool hf = json_int(line, "from", &from);
        trace_records(out, outlen, from, (int)count, hf);
    }
    else if (!strcmp(cmd, "dispatch_miss_info"))
        snprintf(out, outlen, "{\"ok\":true,\"count\":0,\"last_addr\":0,\"last_frame\":0,\"unique\":[]}");
    else if (!strcmp(cmd, "quit")) { snprintf(out, outlen, "{\"ok\":true,\"bye\":true}"); return 0; }
    else snprintf(out, outlen, "{\"ok\":false,\"error\":\"unknown cmd '%s'\"}", cmd);
    return 1;
}

static int g_port = 4381;
static void server_loop() {
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == BAD_SOCK) return;
    int yes = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)g_port);
    if (bind(ls, (sockaddr *)&a, sizeof a) != 0) { fprintf(stderr, "[oracle] bind(:%d) failed\n", g_port); close_sock(ls); return; }
    listen(ls, 1);
    fprintf(stderr, "[oracle] TCP debug server live on 127.0.0.1:%d\n", g_port);
    for (;;) {
        sock_t cs = accept(ls, nullptr, nullptr);
        if (cs == BAD_SOCK) break;
        static char in[2048]; static char outb[65536]; int inlen = 0;
        for (;;) {
            char ch; int r = (int)recv(cs, &ch, 1, 0);
            if (r <= 0) break;
            if (ch == '\n') {
                in[inlen] = 0;
                int keep = handle_line(in, outb, sizeof outb);
                int ol = (int)strlen(outb); outb[ol++] = '\n';
                send(cs, outb, ol, 0); inlen = 0;
                if (!keep) break;
            } else if (ch != '\r' && inlen < (int)sizeof in - 1) in[inlen++] = ch;
        }
        close_sock(cs);
    }
    close_sock(ls);
}

int main(int argc, char **argv) {
    const char *rom_path = nullptr;
    uint64_t steps = 100000; bool pal = false, hold = false;
    uint64_t stop_seq = 0; bool have_stop_seq = false;
    const char *inject_spec = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--pal")) pal = true;
        else if (!strcmp(argv[i], "--ntsc")) pal = false;
        else if (!strcmp(argv[i], "--hold")) hold = true;
        /* --stop-seq N (COSIM-SPEC.md §5/§6): run to seq N, then hold with
         * rings live for drill-down (get_registers/read_mem/
         * cosim_full_ram_hash) — mirrors the native cdi_fault_hold. Never
         * used to lockstep-synchronize the two sides; the coordinator only
         * queries rings. */
        else if (!strcmp(argv[i], "--stop-seq") && i + 1 < argc) { stop_seq = strtoull(argv[++i], nullptr, 0); have_stop_seq = true; }
        /* --cosim-inject "<seq>:<kind>:<idx>:<xorhex>" (COSIM-SPEC.md §5): a
         * startup knob, applied once in capture() when the run reaches the
         * given seq (never a mid-run command — keeps the free-run doctrine). */
        else if (!strcmp(argv[i], "--cosim-inject") && i + 1 < argc) inject_spec = argv[++i];
        else if (argv[i][0] != '-') rom_path = argv[i];
    }
#ifdef CDI_COSIM
    if (!inject_spec) inject_spec = getenv("CDI_COSIM_INJECT");
    if (inject_spec && !parse_cosim_inject(inject_spec))
        fprintf(stderr, "[oracle] malformed --cosim-inject/CDI_COSIM_INJECT spec '%s' (want <seq>:<ram|reg>:<idx>:<xorhex>) — injection disabled\n", inject_spec);
#endif
    printf("CdiOracle — CeDImu core as the CD-i behavioral oracle\n");
    if (!rom_path) {
        fprintf(stderr, "usage: CdiOracle <cdrtos.rom> [--port 4381] [--steps N] [--stop-seq N] [--ntsc|--pal] [--hold]\n"
                         "                 [--cosim-inject <seq>:<ram|reg>:<idx>:<xorhex>]\n");
        return 1;
    }

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    std::ifstream f(rom_path, std::ios::binary);
    if (!f) { fprintf(stderr, "[oracle] cannot open %s\n", rom_path); return 1; }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    printf("  ROM: %s (%zu KB)\n", rom_path, rom.size() / 1024);

    bool has32k;
    try {
        OS9::BIOS probe(std::span<const uint8_t>(rom.data(), rom.size()));
        has32k = !probe.Has8KBNVRAM();
    } catch (const std::exception &e) { fprintf(stderr, "[oracle] BIOS parse failed: %s\n", e.what()); return 1; }

    CDIConfig config = defaultConfig;
    config.PAL = pal;
    config.has32KBNVRAM = has32k;

    /* Empty NVRAM span -> the timekeeper fills 0xFF defaults (deterministic).
     * A non-empty span must be the exact chip size (DS1216 32776 / M48T08 8192). */
    try {
        g_cdi = (CDI::NewCDI(Boards::AutoDetect, std::span<const uint8_t>(rom.data(), rom.size()),
                             std::span<const uint8_t>(),
                             config, Callbacks(), CDIDisc())).release();
    } catch (const std::exception &e) { fprintf(stderr, "[oracle] NewCDI failed: %s\n", e.what()); return 1; }

    g_cdi->m_cpu.SetEmulationSpeed(1e6);   /* neutralise the real-time pacing sleep */

#ifdef CDI_COSIM
    /* MC-CDI-016: bind the RAM-bank hasher to this CDI instance and mark
     * every page dirty (forces a full hash on the first cdi_cosim_ram_hash()
     * call). Must run before the first capture() AND before the server
     * thread starts accepting cosim_full_ram_hash/trace requests. */
    cdi_cosim_state_init(g_cdi);
#endif

    std::thread srv(server_loop);
    srv.detach();

    printf("  board: %s  nvram: %s  %s\n", g_cdi->m_boardName.c_str(),
           has32k ? "32KB" : "8KB", pal ? "PAL" : "NTSC");
    const uint64_t target = have_stop_seq ? stop_seq : steps;
    printf("[oracle] stepping %llu instructions ...\n", (unsigned long long)target);
    fflush(stdout);

    for (uint64_t i = g_seq; i < target; i++) {
        auto pre = g_cdi->m_cpu.GetCPURegisters();  /* entry-state registers */
#ifdef CDI_COSIM
        /* Apply the armed injection at the same pre-Run instant native applies
         * it (debug_trace_block runs cdi_cosim_maybe_inject before the
         * instruction executes), then snapshot the pre-instruction RAM hashes,
         * so this record's regs / usp / ssp / ram*_h are all consistent pre-N
         * state and align byte-for-byte with the native ring (COSIM-SPEC §1). */
        if (g_inject_armed && !g_inject_done && g_seq == g_inject_seq) {
            if (g_inject_is_ram) cdi_cosim_inject_ram(g_inject_idx, (uint8_t)g_inject_xor);
            else                 cdi_cosim_inject_reg((int)g_inject_idx, g_inject_xor);
            g_inject_done = true;
            pre = g_cdi->m_cpu.GetCPURegisters();   /* reflect the injected reg */
        }
        g_pre_ram0 = cdi_cosim_ram_hash(0);
        g_pre_ram1 = cdi_cosim_ram_hash(1);
#endif
        g_cdi->m_cpu.Run(false);                    /* execute one instruction */
        capture(g_cdi->m_cpu.currentPC, pre);       /* currentPC = the instr just executed */
    }
    printf("[oracle] captured %llu instructions; first PC=$%08X\n",
           (unsigned long long)g_seq, g_ring[0].pc);

    /* --stop-seq always parks and keeps serving TCP (the whole point is a
     * frozen, drillable seq for the coordinator); --hold does the same after
     * a plain --steps run. The TCP server thread above is already live and
     * keeps answering get_registers/read_mem/trace/cosim_full_ram_hash. */
    if (hold || have_stop_seq) {
        printf("[oracle] %s: trace live on :%d, parked at seq %llu. Ctrl-C to exit.\n",
               have_stop_seq ? "--stop-seq" : "--hold", g_port, (unsigned long long)g_seq);
        fflush(stdout);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
