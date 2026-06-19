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
struct TraceRec { uint64_t seq; uint32_t pc, sr, d[8], a[8], a7top; };
static constexpr uint32_t RING = 1u << 18;
static constexpr uint32_t MASK = RING - 1;
static std::vector<TraceRec> g_ring(RING);
static uint64_t g_seq = 0;
static CDI *g_cdi = nullptr;

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
static void capture(uint32_t pc, const std::map<Reg, uint32_t> &r) {
    TraceRec &t = g_ring[g_seq & MASK];
    t.seq = g_seq;
    t.pc = pc;
    t.sr = r.at(Reg::SR);
    for (int i = 0; i < 8; i++) t.d[i] = r.at(static_cast<Reg>(static_cast<int>(Reg::D0) + i));
    for (int i = 0; i < 8; i++) t.a[i] = r.at(static_cast<Reg>(static_cast<int>(Reg::A0) + i));
    t.a7top = peek_be32(t.a[7]);   /* stack top at instruction entry */
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
        n += snprintf(out + n, outlen - n, ",\"a7top\":%u", recs[i].a7top);
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
        int n = snprintf(out, outlen, "{\"ok\":true,\"pc\":%u,\"sr\":%u,\"usp\":%u",
                         regs.at(Reg::PC), regs.at(Reg::SR), regs.at(Reg::USP));
        for (int i = 0; i < 8; i++) n += snprintf(out + n, outlen - n, ",\"d%d\":%u", i, regs.at(static_cast<Reg>(static_cast<int>(Reg::D0) + i)));
        for (int i = 0; i < 8; i++) n += snprintf(out + n, outlen - n, ",\"a%d\":%u", i, regs.at(static_cast<Reg>(static_cast<int>(Reg::A0) + i)));
        snprintf(out + n, outlen - n, "}");
    }
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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--pal")) pal = true;
        else if (!strcmp(argv[i], "--ntsc")) pal = false;
        else if (!strcmp(argv[i], "--hold")) hold = true;
        else if (argv[i][0] != '-') rom_path = argv[i];
    }
    printf("CdiOracle — CeDImu core as the CD-i behavioral oracle\n");
    if (!rom_path) { fprintf(stderr, "usage: CdiOracle <cdrtos.rom> [--port 4381] [--steps N] [--ntsc|--pal] [--hold]\n"); return 1; }

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

    std::thread srv(server_loop);
    srv.detach();

    printf("  board: %s  nvram: %s  %s\n", g_cdi->m_boardName.c_str(),
           has32k ? "32KB" : "8KB", pal ? "PAL" : "NTSC");
    printf("[oracle] stepping %llu instructions ...\n", (unsigned long long)steps);
    fflush(stdout);

    for (uint64_t i = 0; i < steps; i++) {
        auto pre = g_cdi->m_cpu.GetCPURegisters();  /* entry-state registers */
        g_cdi->m_cpu.Run(false);                    /* execute one instruction */
        capture(g_cdi->m_cpu.currentPC, pre);       /* currentPC = the instr just executed */
    }
    printf("[oracle] captured %llu instructions; first PC=$%08X\n",
           (unsigned long long)g_seq, g_ring[0].pc);

    if (hold) {
        printf("[oracle] --hold: trace live on :%d. Ctrl-C to exit.\n", g_port);
        fflush(stdout);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
