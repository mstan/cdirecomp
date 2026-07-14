/*
 * slave.c — IKAT input/serial-gate MCU (HLE), ported from CeDImu HLE::IKAT.
 *
 * The CD-i input MCU sits at $310000 on the odd bytes; register index =
 * (addr - $310000) >> 1. On Mono-3/4 (cdi490a = Mono-4, implemented by the
 * Mono3 board in CeDImu) and Mono-2 this gate is the IKAT (the "SLAVE" naming
 * is Mono-1/2 legacy). The CD-RTOS boot talks to it over four byte channels
 * (A-D) with a command/response protocol: it queries the IKAT firmware
 * version, the boot mode (player vs service shell), the video standard
 * (NTSC/PAL), and disc status.
 *
 * 15 host-visible registers:
 *   0-3   CHx_IN   host writes command bytes
 *   4-7   CHx_OUT  host reads response bytes
 *   8-11  CHx_SR   channel status (bit0/bit4 = ready/empty flags)
 *   12    ISR      interrupt status
 *   13    IMR      interrupt mask
 *   14    Mode/YCR (writes are no-ops)
 *
 * CeDImu's IKAT HLE supplies the protocol/timing baseline. Where that HLE is
 * incomplete for ready media or disagrees with this literal cdi490 ROM's
 * driver wiring, the ROM contract wins. Channel-D disc responses are 2-frame
 * delayed and the maneuvering-device channel runs on a 25-ms emulated-time
 * cadence, both driven by slave_increment_time().
 *
 * TODO MC-CDI-014 / MC-CDI-023.
 */
#include "cdi_runtime.h"
#include "cdi_media.h"
#include "debug_server.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- register map ---- */
enum { CHA_IN=0, CHB_IN, CHC_IN, CHD_IN,
       CHA_OUT, CHB_OUT, CHC_OUT, CHD_OUT,
       CHA_SR,  CHB_SR,  CHC_SR,  CHD_SR,
       ISR, IMR, MODE, IKAT_NREG };
enum { CHA=0, CHB, CHC, CHD };

#define UNSET_REMTY(reg) (s_reg[reg] &= 0x01)
#define SET_RTEMTY(reg)  (s_reg[reg] |= 0x11)
#define CHANNEL(addr)    ((addr) & 3)

static const int INT_MASK[4] = { 0x02, 0x08, 0x20, 0x80 };

/* Video standard: Hotel Mario USA is NTSC (PAL=0). responseCF6[2] = PAL+1. */
static int s_pal = 0;

static uint8_t s_reg[IKAT_NREG];

/* Per-channel input (host->MCU command accumulation) and output (MCU->host
 * response) byte queues. Commands/responses are <= 4 bytes. */
static uint8_t s_in[4][8];   static int s_in_len[4];
static uint8_t s_out[4][8];  static int s_out_pos[4]; static int s_out_len[4];

/* 2-frame-delayed channel responses (disc queries). */
static uint8_t        s_delayed_rsp[4][8]; static int s_delayed_len[4];
static uint8_t        s_delayed_pending[4];
static uint64_t       s_delayed_frame[4];

/* Command responses (https://github.com/cdifan/cdichips/blob/master/mc6805ikat.md). */
static const uint8_t responseCF0[3] = { 0xA5, 0xF0, 0x7F }; /* IKAT release number   */
/* Pointing-device IDs: 'M' is the documented relative-device code and the
 * second port is empty. A5 routes this shared channel-C response to the
 * correct CD-RTOS interrupt consumer on Mono-III/IV. */
static const uint8_t responseCF3[4] = { 0xA5, 0xF3, 'M', 0 };
static       uint8_t responseCF6[4] = { 0xA5, 0xF6, 1, 0xFF }; /* video std: [2]=PAL+1 */
static const uint8_t responseCF4[3] = { 0xA5, 0xF4, 0 };   /* boot mode: 0=player shell */
static const uint8_t responseCF7[3] = { 0xA5, 0xF7, 0 };   /* CD60 low-level test (0=ok) */
static const uint8_t responseCF8[3] = { 0xA5, 0xF8, 0 };   /* DSIC2 low-level test (0=ok) */
static const uint8_t responseDB1[4] = { 0xB1, 0, 2, 0 };       /* disc base     */
static const uint8_t responseDB2[4] = { 0xB2, 0x20, 0, 0x10 }; /* disc select   */

static int  s_inited = 0;
static int  s_disc_present;
static uint64_t s_media_generation;

/* PointingDevice::Class::Maneuvering state. The frontend/debug thread
 * only publishes the desired buttons atomically; the CPU thread owns all IKAT
 * queues/registers and turns that state into protocol packets on emulated time.
 * This avoids cross-thread mutation of the guest-visible device. */
static atomic_uint s_input_mask;
static uint32_t    s_last_input_mask;
/* ISlave::IncrementTime takes size_t in CeDImu, so Mono3's fractional
 * CPU-quantum nanoseconds are truncated before the PointingDevice timer sees
 * them.  Preserve that per-quantum truncation here: accumulating the double
 * directly makes the 25-ms pointer cadence run several milliseconds early
 * after millions of CPU/STOP quanta. */
static uint64_t    s_pointer_time_ns;
static int         s_consecutive_cursor_packets;

#define INPUT_SCHEDULE_CAP 64
typedef struct {
    uint64_t frame;
    uint32_t mask;
} InputScheduleEntry;
static InputScheduleEntry s_input_schedule[INPUT_SCHEDULE_CAP];
static int s_input_schedule_count;
static int s_input_schedule_pos;

#define IKAT_EVENT_CAP 256
static CdiIkatEvent s_events[IKAT_EVENT_CAP];
static uint64_t s_event_total;

static void record_event(uint8_t type, int channel, const uint8_t *data, int length) {
    CdiIkatEvent *e = &s_events[s_event_total % IKAT_EVENT_CAP];
    memset(e, 0, sizeof *e);
    e->seq = s_event_total++;
    e->trace_seq = debug_trace_sequence();
    e->frame = g_frame_count;
    e->cycles = g_total_cycles;
    e->pc = g_cpu.PC;
    e->type = type;
    e->channel = (uint8_t)channel;
    if (length > 8) length = 8;
    e->length = (uint8_t)length;
    if (length > 0 && data) memcpy(e->data, data, (size_t)length);
}

int slave_debug_events(CdiIkatEvent *out, int capacity, uint64_t *total) {
    uint64_t end = s_event_total;
    uint64_t begin = end > IKAT_EVENT_CAP ? end - IKAT_EVENT_CAP : 0;
    if ((uint64_t)capacity < end - begin) begin = end - (uint64_t)capacity;
    int count = 0;
    for (uint64_t seq = begin; seq < end; seq++) out[count++] = s_events[seq % IKAT_EVENT_CAP];
    if (total) *total = end;
    return count;
}

#define IKAT_POINTER_PACKET_NS 25000000.0

void cdi_input_set(uint32_t mask) {
    const uint32_t valid = CDI_INPUT_LEFT | CDI_INPUT_UP | CDI_INPUT_RIGHT |
                           CDI_INPUT_DOWN | CDI_INPUT_BTN1 | CDI_INPUT_BTN2;
    atomic_store_explicit(&s_input_mask, mask & valid, memory_order_release);
}

uint32_t cdi_input_get(void) {
    return atomic_load_explicit(&s_input_mask, memory_order_acquire);
}

int cdi_input_schedule_configure(const char *spec) {
    s_input_schedule_count = 0;
    s_input_schedule_pos = 0;
    if (!spec || !*spec) return 0;

    size_t length = strlen(spec);
    char *copy = (char *)malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, spec, length + 1);

    char *cursor = copy;
    uint64_t previous = 0;
    int have_previous = 0;
    while (*cursor) {
        if (s_input_schedule_count >= INPUT_SCHEDULE_CAP) {
            free(copy);
            s_input_schedule_count = 0;
            return 0;
        }
        char *frame_end = NULL;
        if (*cursor == '-') {
            free(copy);
            s_input_schedule_count = 0;
            return 0;
        }
        errno = 0;
        unsigned long long frame = strtoull(cursor, &frame_end, 0);
        if (errno == ERANGE || frame_end == cursor || *frame_end != ':') {
            free(copy);
            s_input_schedule_count = 0;
            return 0;
        }
        char *mask_start = frame_end + 1;
        char *mask_end = NULL;
        if (*mask_start == '-') {
            free(copy);
            s_input_schedule_count = 0;
            return 0;
        }
        errno = 0;
        unsigned long mask = strtoul(mask_start, &mask_end, 0);
        if (errno == ERANGE || mask_end == mask_start ||
            (*mask_end != ',' && *mask_end != '\0') ||
            (have_previous && (uint64_t)frame <= previous) || mask > 0x3Fu) {
            free(copy);
            s_input_schedule_count = 0;
            return 0;
        }
        s_input_schedule[s_input_schedule_count++] =
            (InputScheduleEntry){(uint64_t)frame, (uint32_t)mask};
        previous = (uint64_t)frame;
        have_previous = 1;
        if (*mask_end == ',') {
            cursor = mask_end + 1;
            if (!*cursor) {
                free(copy);
                s_input_schedule_count = 0;
                return 0;
            }
        } else {
            cursor = mask_end;
        }
    }
    free(copy);
    return s_input_schedule_count > 0;
}

void cdi_input_schedule_advance(uint64_t completed_frame) {
    while (s_input_schedule_pos < s_input_schedule_count &&
           s_input_schedule[s_input_schedule_pos].frame <= completed_frame) {
        cdi_input_set(s_input_schedule[s_input_schedule_pos].mask);
        s_input_schedule_pos++;
    }
}

void slave_debug_state(uint8_t regs[15], uint8_t out_remaining[4],
                       double *pointer_time_ns, int *cursor_packets) {
    if (!s_inited) {
        memset(regs, 0, 15);
        memset(out_remaining, 0, 4);
        *pointer_time_ns = 0.0;
        *cursor_packets = 0;
        return;
    }
    memcpy(regs, s_reg, 15);
    for (int ch = 0; ch < 4; ch++)
        out_remaining[ch] = (uint8_t)(s_out_len[ch] - s_out_pos[ch]);
    *pointer_time_ns = (double)s_pointer_time_ns;
    *cursor_packets = s_consecutive_cursor_packets;
}

static void ikat_init(void) {
    memset(s_reg, 0, sizeof s_reg);
    memset(s_in_len, 0, sizeof s_in_len);
    memset(s_out_pos, 0, sizeof s_out_pos);
    memset(s_out_len, 0, sizeof s_out_len);
    memset(s_delayed_rsp, 0, sizeof s_delayed_rsp);
    memset(s_delayed_pending, 0, sizeof s_delayed_pending);
    responseCF6[2] = (uint8_t)(s_pal + 1);
    for (int reg = CHA_SR; reg <= CHD_SR; reg++) SET_RTEMTY(reg);
    atomic_store_explicit(&s_input_mask, 0, memory_order_relaxed);
    s_last_input_mask = 0;
    s_pointer_time_ns = 0;
    s_consecutive_cursor_packets = 0;
    memset(s_events, 0, sizeof s_events);
    s_event_total = 0;
    s_disc_present = cdi_media_present();
    s_media_generation = cdi_media_generation();
    s_inited = 1;
}

static void set_int(int ch) {
    s_reg[ISR] |= (uint8_t)INT_MASK[ch];
    if (s_reg[IMR] & INT_MASK[ch]) {
        record_event(CDI_IKAT_IRQ, ch, NULL, 0);
        cdi_irq_raise(2);            /* IKAT asserts the external level-2 line */
    }
}

static void out_set(int ch, const uint8_t *rsp, int n) {
    memcpy(s_out[ch], rsp, (size_t)n);
    s_out_pos[ch] = 0;
    s_out_len[ch] = n;
    record_event(CDI_IKAT_RESPONSE, ch, rsp, n);
}
static int out_size(int ch) { return s_out_len[ch] - s_out_pos[ch]; }

static void delay_rsp(int ch, const uint8_t *rsp, int n) {
    memcpy(s_delayed_rsp[ch], rsp, (size_t)n);
    s_delayed_len[ch]   = n;
    s_delayed_pending[ch] = 1;
    s_delayed_frame[ch] = g_frame_count + 2;
}

/* IKAT disc status. Keep the Mono-III/IV four-byte response shape used by
 * this BIOS; its B0 handler consumes the bytes as fields, not one status word. */
static void disc_status_response(uint8_t rsp[4]) {
    rsp[0] = 0xB0;
    if (cdi_media_present()) {
        /* The four bytes are not a single big-endian status word.  CD-RTOS's
         * cdislave B0 handler consumes byte 2 as the drive state and byte 3 as
         * the media/event code; leaving byte 3 at zero reports "no event" and
         * cannot start the inserted-media state machine.  MAME's real-disc
         * response is B0 00 02 15, which matches those literal ROM fields. */
        rsp[1] = 0x00; rsp[2] = 0x02; rsp[3] = 0x15;
    } else {
        /* CeDImu/cdiemu no-media value used by the matched Mono-IV boot. */
        rsp[1] = 0x02; rsp[2] = 0x10; rsp[3] = 0x00;
    }
}

static void process_command_c(void) {
    record_event(CDI_IKAT_COMMAND, CHC, s_in[CHC], s_in_len[CHC]);
    switch (s_in[CHC][0]) {
    case 0xF0: s_in_len[CHC]=0; out_set(CHC, responseCF0, 3); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    case 0xF3: s_in_len[CHC]=0; out_set(CHC, responseCF3, 4); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    case 0xF4: s_in_len[CHC]=0; out_set(CHC, responseCF4, 3); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    case 0xF6: s_in_len[CHC]=0; out_set(CHC, responseCF6, 4); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    case 0xF7: s_in_len[CHC]=0; out_set(CHC, responseCF7, 3); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    case 0xF8: s_in_len[CHC]=0; out_set(CHC, responseCF8, 3); UNSET_REMTY(CHC_SR); set_int(CHC); break;
    default:   s_in_len[CHC]=0; break;
    }
}

static void process_command_d(void) {
    uint8_t status[4];
    switch (s_in[CHD][0]) {
    case 0xA1: if (s_in_len[CHD]==4) { record_event(CDI_IKAT_COMMAND, CHD, s_in[CHD], 4); s_in_len[CHD]=0; disc_status_response(status); delay_rsp(CHD, status, 4); } break;
    case 0xB0: if (s_in_len[CHD]==4) { record_event(CDI_IKAT_COMMAND, CHD, s_in[CHD], 4); s_in_len[CHD]=0; disc_status_response(status); delay_rsp(CHD, status, 4); } break;
    case 0xB1: record_event(CDI_IKAT_COMMAND, CHD, s_in[CHD], s_in_len[CHD]); s_in_len[CHD]=0; out_set(CHD, responseDB1, 4); UNSET_REMTY(CHD_SR); set_int(CHD); break;
    case 0xB2: if (s_in_len[CHD]==4) { record_event(CDI_IKAT_COMMAND, CHD, s_in[CHD], 4); s_in_len[CHD]=0; delay_rsp(CHD, responseDB2, 4); } break;
    case 0xC0: record_event(CDI_IKAT_COMMAND, CHD, s_in[CHD], s_in_len[CHD]); s_in_len[CHD]=0; break;
    case 0xE0: case 0xE1:
        if (s_in_len[CHD] == 4) { record_event(CDI_IKAT_COMMAND, CHD, s_in[CHD], 4); s_in_len[CHD]=0; }
        break;
    default:   record_event(CDI_IKAT_COMMAND, CHD, s_in[CHD], s_in_len[CHD]); s_in_len[CHD]=0; break;
    }
}

/* Generate the four-byte Class::Maneuvering pointer message. Directional
 * input repeats every 25 ms and accelerates 2,2,4,4,6,6,8,8; button changes
 * produce a packet on the next cadence edge.  This cdi490 ROM's pt1driv
 * enables IMR bit 1 and reads channel A; MAME's SLAVE model routes its pointer
 * report there as well.  CeDImu's Mono3 HLE currently queues the same packet
 * on channel B, which leaves it permanently masked from this literal BIOS. */
static void pointer_tick(void) {
    uint32_t input = cdi_input_get();
    int directional = (input & (CDI_INPUT_LEFT | CDI_INPUT_UP |
                                CDI_INPUT_RIGHT | CDI_INPUT_DOWN)) != 0;
    if (directional)
        s_consecutive_cursor_packets++;
    else
        s_consecutive_cursor_packets = 0;

    int speed = s_consecutive_cursor_packets + (s_consecutive_cursor_packets & 1);
    if (speed > 8) speed = 8;
    int x = 0, y = 0;
    if      (input & CDI_INPUT_LEFT)  x = -speed;
    else if (input & CDI_INPUT_RIGHT) x = speed;
    if      (input & CDI_INPUT_UP)    y = -speed;
    else if (input & CDI_INPUT_DOWN)  y = speed;

    uint32_t changed_buttons = (input ^ s_last_input_mask) &
                               (CDI_INPUT_BTN1 | CDI_INPUT_BTN2);
    if (directional || changed_buttons) {
        uint8_t msg[4];
        msg[0] = (uint8_t)(0x40 |
                 ((input & CDI_INPUT_BTN1) ? 0x20 : 0) |
                 ((input & CDI_INPUT_BTN2) ? 0x10 : 0) |
                 ((y >> 4) & 0x0C) | ((x >> 6) & 0x03));
        msg[1] = (uint8_t)(x & 0x3F);
        msg[2] = (uint8_t)(y & 0x3F);
        msg[3] = 0;
        out_set(CHA, msg, 4);
        UNSET_REMTY(CHA_SR);
        set_int(CHA);
    }
    s_last_input_mask = input;
}

void slave_increment_time(double ns) {
    if (!s_inited) ikat_init();
    int disc_present = cdi_media_present();
    uint64_t media_generation = cdi_media_generation();
    if (media_generation != s_media_generation) {
        uint8_t status[4];
        s_media_generation = media_generation;
        s_disc_present = disc_present;
        uint8_t state = (uint8_t)disc_present;
        record_event(CDI_IKAT_MEDIA, CHD, &state, 1);
        disc_status_response(status);
        out_set(CHD, status, 4);
        UNSET_REMTY(CHD_SR);
        set_int(CHD);
    }
    s_pointer_time_ns += (uint64_t)ns;
    if (s_pointer_time_ns >= (uint64_t)IKAT_POINTER_PACKET_NS) {
        s_pointer_time_ns -= IKAT_POINTER_PACKET_NS;
        pointer_tick();
    }

    for (int ch = CHA; ch <= CHD; ch++) {
        if (s_delayed_pending[ch] && s_delayed_frame[ch] == g_frame_count) {
            out_set(ch, s_delayed_rsp[ch], s_delayed_len[ch]);
            UNSET_REMTY(CHA_SR + ch);
            set_int(ch);
            s_delayed_pending[ch] = 0;
            s_delayed_frame[ch] = 0;
        }
    }
}

static uint8_t ikat_get(uint8_t reg) {
    int ch = CHANNEL(reg);
    if (reg >= CHA_OUT && reg <= CHD_OUT && out_size(ch) > 0) {
        s_reg[reg] = s_out[ch][s_out_pos[ch]++];
        record_event(CDI_IKAT_READ, ch, &s_reg[reg], 1);
        if (out_size(ch) == 0) {
            s_reg[ISR] &= (uint8_t)~(1 << (1 + 2 * ch));
            SET_RTEMTY(CHA_SR + ch);
        } else {
            UNSET_REMTY(CHA_SR + ch);
        }
    }
    return s_reg[reg];
}

static void ikat_set(uint8_t reg, uint8_t data) {
    if (reg == IMR) {
        s_reg[IMR] = data;
        if (s_reg[ISR] & s_reg[IMR]) cdi_irq_raise(2);
    } else if (reg <= CHD_IN) {           /* CHA_IN..CHD_IN */
        s_reg[reg] = data;
        int ch = CHANNEL(reg);
        if (s_in_len[ch] < (int)sizeof s_in[ch]) s_in[ch][s_in_len[ch]++] = data;
        if (reg == CHC_IN) process_command_c();
        else if (reg == CHD_IN) process_command_d();
    }
    /* reg 14 (Mode/YCR) and the OUT/SR/ISR registers: writes are ignored. */
}

/* ---- bus interface ---- */
static int addr_to_reg(uint32_t addr) {
    /* IKAT lives on the odd bytes; register index = (addr - base) >> 1. */
    return (int)((addr - CDI_SLAVE_BASE) >> 1);
}

uint32_t slave_read(uint32_t addr, int size) {
    if (!s_inited) ikat_init();
    int reg = addr_to_reg(addr);
    if (reg < 0 || reg >= IKAT_NREG) {
        fprintf(stderr, "[ikat] read%d @ $%08X -> reg %d out of range\n", size*8, addr, reg);
        return 0;
    }
    return ikat_get((uint8_t)reg);
}

void slave_write(uint32_t addr, uint32_t val, int size) {
    if (!s_inited) ikat_init();
    int reg = addr_to_reg(addr);
    if (reg < 0 || reg >= IKAT_NREG) {
        fprintf(stderr, "[ikat] write%d @ $%08X = $%X -> reg %d out of range\n",
                size*8, addr, val, reg);
        return;
    }
    ikat_set((uint8_t)reg, (uint8_t)val);
}
