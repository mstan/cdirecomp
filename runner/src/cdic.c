/*
 * Philips MCD221 CD Interface and Audio Processor (CIAP), as fitted to the
 * Mono-III/IV CD-i boards.
 *
 * This clean-room model implements the host-visible data path used by CD-RTOS:
 * firmware/register RAM, alternating 2340-byte sector buffers, buffer
 * ownership, selection, status acknowledgement, and the programmable
 * vectored interrupt.  Drive positioning remains an IKAT responsibility;
 * slave.c supplies absolute MSF positions and the CIAP clocks sectors at the
 * physical 75 Hz frame cadence.
 */
#include "cdi_runtime.h"
#include "cdi_media.h"
#include "cdi_audio.h"
#include "debug_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CIAP_DATA0 = 0x1200u,
    CIAP_DATA1 = 0x1BC2u,
    /* Each data buffer is followed by the sector's subcode-Q image (the
     * "locator" the CD-RTOS record path tracks drive position with):
     * $1200+$924 = $1B24 and $1BC2+$924 = $24E6. */
    CIAP_QBUF0 = 0x1B24u,
    CIAP_QBUF1 = 0x24E6u,
    CIAP_IER = 0x2584u,
    CIAP_ISR = 0x2586u,
    CIAP_TACS = 0x2588u,
    CIAP_TCM1 = 0x258Cu,
    CIAP_ACM2 = 0x2590u,
    CIAP_FILE = 0x2592u,
    CIAP_BMAN = 0x2594u,
    CIAP_CCR = 0x2596u,
    CIAP_APCR = 0x25A6u,
    CIAP_A_SHDW = 0x259Au,
    CIAP_ASTAT = 0x25AAu,
    CIAP_AP_POINTER = 0x25B4u,
    CIAP_ICR = 0x25C0u,
    CIAP_DMACTL = 0x25C2u,
    CIAP_ID = 0x25C4u,
    CIAP_DLOAD = 0x25FEu,
    CIAP_REGISTER_BYTES = 0x2600u,
    CIAP_APERTURE_BYTES = 0x4000u,
    CIAP_SECTOR_BYTES = 2340u,
    CIAP_ISR_DATA = 0x0001u,
    CIAP_ISR_QREADY = 0x0004u,
    /* The record driver maps bit 9 to its trigger flag and bit 10 to EOF
     * ($428C1C). They notify the stream handler even when channel selection
     * routes the payload away from the host; they are NOT Q-buffer bits. */
    CIAP_ISR_STREAM_TRIGGER = 0x0200u,
    CIAP_ISR_FILE_END = 0x0400u,
    CIAP_BMAN_DATA0 = 0x0004u,
    CIAP_BMAN_DATA1 = 0x0008u,
    CIAP_BMAN_Q0 = 0x0010u,
    CIAP_BMAN_Q1 = 0x0020u,
    /* Locator/subcode ownership bits ($10/$20/$40/$80) are acknowledged
     * write-one-to-clear: the ROM ISR always writes back the accumulated
     * mask of a whole pair ($30 or $C0), unlike the DATA XOR handoff. */
    CIAP_BMAN_ACK_CLEAR = 0x00F0u,
    CIAP_CCR_ASEL = 0x0008u,
    CIAP_CCR_MEMORY_PLAY = 0x0040u,
    CIAP_CCR_STARTD = 0x00C4u,
    /* Locator-reporting stream starts. CD-RTOS arms them behind the IKAT
     * on-target report: $3000/$0010 for the record path (phase 2) and
     * $7000/$0044 for the play/read paths (the retry timeouts also re-arm
     * with $3000/$0094 after RESET); the $3000/$7000 prefixes and the $0080
     * chaser are stored untouched. */
    CIAP_CCR_STARTQ_RECORD = 0x0010u,
    CIAP_CCR_STARTQ_PLAY = 0x0044u,
    CIAP_CCR_STARTQ_RETRY = 0x0094u,
    CIAP_CCR_RESET = 0x0100u,
    /* Idle work-ready word the CIAP firmware presents in ISR between core
     * start (DLOAD exit / power-on) and the first stream arming. */
    CIAP_ISR_FIRMWARE_IDLE = 0x0009u,
    CIAP_APCR_FINISH = 0x0020u,
    CIAP_APCR_INTNOW = 0x00A0u,
    /* Interrupt-enable command modifier: the driver's transport commands
     * ($142 play-continue, $140 record-engine arm, $101) set bit 8 and wait
     * for the completion interrupt before advancing their state machine. */
    CIAP_APCR_IE = 0x0100u,
    CIAP_APCR_ACK = 0x0200u,
    CD_I_SUBMODE_EOF = 0x80u,
    CD_I_SUBMODE_TRIGGER = 0x10u,
    CD_I_SUBMODE_DATA = 0x08u,
    CD_I_SUBMODE_AUDIO = 0x04u,
    CD_I_SUBMODE_VIDEO = 0x02u,
    CD_I_SUBMODE_EOR = 0x01u,
    CIAP_EVENT_CAPACITY = 1u << 16,
    CIAP_EVENT_INDEX_MASK = CIAP_EVENT_CAPACITY - 1u
};

typedef struct {
    uint8_t memory[CIAP_APERTURE_BYTES];
    CdiCiapEvent events[CIAP_EVENT_CAPACITY];
    uint64_t event_count;
    uint64_t sector_elapsed_ns;
    uint64_t data_deliveries, locator_deliveries, held_ticks;
    uint32_t drive_lba;
    uint32_t last_lba;
    uint8_t last_file;
    uint8_t last_channel;
    uint8_t last_submode;
    uint8_t last_coding;
    uint8_t last_selected;
    uint8_t next_data_buffer;
    uint8_t next_q_buffer;
    uint8_t drive_positioned;
    uint8_t audio_positioned;
    uint8_t data_running;
    uint8_t data_path_armed;
    uint8_t firmware_idle;
    uint8_t q_reporting;
    uint8_t selection_active;
    uint8_t selection_header_pending;
    uint8_t ap_completion_pending;
    uint64_t ap_completion_delay_ns;
    uint64_t audio_remaining_ns;
    uint64_t audio_duration_ns;
    uint16_t audio_format[2];
    uint8_t audio_playing;
    uint8_t audio_buffer;
    uint8_t audio_active;
    uint8_t audio_finish_requested;
    uint8_t audio_select_pending;
    int initialized;
} CiapDevice;

static CiapDevice ciap;

static uint16_t read_word(unsigned offset) {
    return (uint16_t)(((uint16_t)ciap.memory[offset] << 8) |
                      ciap.memory[offset + 1u]);
}

static void store_word(unsigned offset, uint16_t value) {
    ciap.memory[offset] = (uint8_t)(value >> 8);
    ciap.memory[offset + 1u] = (uint8_t)value;
}

/* Both host buffers owned inhibits payload delivery. The external drive
 * continues to advance; locators and file events remain independent. */
static int data_buffers_full(void) {
    return (read_word(CIAP_BMAN) & (CIAP_BMAN_DATA0 | CIAP_BMAN_DATA1))
           == (CIAP_BMAN_DATA0 | CIAP_BMAN_DATA1);
}

static unsigned interrupt_level(void) {
    return read_word(CIAP_ICR) & 7u;
}

static unsigned interrupt_vector(void) {
    return (read_word(CIAP_ICR) >> 3) & 0xFFu;
}

static void clear_interrupt_line(void) {
    unsigned level = interrupt_level();
    if (level) cdi_irq_clear((uint8_t)level);
}

static void assert_interrupt_line(void) {
    uint16_t enabled = read_word(CIAP_IER);
    uint16_t status = read_word(CIAP_ISR);
    unsigned level = interrupt_level();
    unsigned vector = interrupt_vector();
    if ((enabled & status) && level)
        cdi_irq_raise_vector((uint8_t)level, (uint8_t)vector);
}

static void initialize_ciap(void) {
    memset(&ciap, 0, sizeof ciap);
    cdi_audio_reset();
    store_word(CIAP_ID, 0xCD02u);
    ciap.firmware_idle = 1;
    ciap.initialized = 1;
}

static void ensure_initialized(void) {
    if (!ciap.initialized) initialize_ciap();
}

static void record_access(uint32_t offset, uint32_t value, int size, int write) {
    CdiCiapEvent *event =
        &ciap.events[ciap.event_count & CIAP_EVENT_INDEX_MASK];
    event->seq = ciap.event_count++;
    event->trace_seq = debug_trace_sequence();
    event->frame = g_frame_count;
    event->cycles = g_total_cycles;
    event->pc = g_cpu.PC;
    event->offset = offset;
    event->value = value;
    event->size = (uint8_t)size;
    event->write = (uint8_t)(write != 0);
}

int cdic_debug_events(CdiCiapEvent *out, int capacity, uint64_t from,
                      uint64_t *total, uint64_t *oldest) {
    uint64_t end = ciap.event_count;
    uint64_t begin = end > CIAP_EVENT_CAPACITY ? end - CIAP_EVENT_CAPACITY : 0;
    int count = 0;

    if (from == UINT64_MAX) {
        uint64_t requested = capacity > 0 ? (uint64_t)capacity : 0;
        from = end > requested ? end - requested : 0;
    }
    if (from < begin) from = begin;
    if (from > end) from = end;
    while (from < end && count < capacity) {
        out[count++] = ciap.events[from & CIAP_EVENT_INDEX_MASK];
        from++;
    }
    if (total) *total = end;
    if (oldest) *oldest = begin;
    return count;
}

void cdic_debug_state(uint32_t *drive_lba, uint32_t *last_lba,
                      uint8_t *file, uint8_t *channel, uint8_t *submode,
                      uint8_t *coding, int *selected, int *running,
                      int *waiting_ack) {
    ensure_initialized();
    *drive_lba = ciap.drive_lba;
    *last_lba = ciap.last_lba;
    *file = ciap.last_file;
    *channel = ciap.last_channel;
    *submode = ciap.last_submode;
    *coding = ciap.last_coding;
    *selected = ciap.last_selected;
    *running = ciap.data_running;
    *waiting_ack = data_buffers_full();
}

static void unknown_read(uint32_t address, uint32_t offset, int size) {
    fprintf(stderr,
            "[ciap] read%d @ $%08X (offset $%X) outside readable registers\n",
            size * 8, address, offset);
    debug_dump_fault_trail("CIAP read outside readable registers");
    if (g_hold_on_fault) cdi_fault_hold();
    abort();
}

void cdic_debug_snapshot(CdiCiapState *out) {
    ensure_initialized();
    out->bman = read_word(CIAP_BMAN);
    out->isr = read_word(CIAP_ISR);
    out->ier = read_word(CIAP_IER);
    out->ccr = read_word(CIAP_CCR);
    out->apcr = read_word(CIAP_APCR);
    out->astat = read_word(CIAP_ASTAT);
    out->channel_mask = (uint32_t)read_word(CIAP_TCM1) |
                       ((uint32_t)read_word(CIAP_ACM2) << 16);
    out->armed = ciap.data_path_armed;
    out->q_reporting = ciap.q_reporting;
    out->selection_active = ciap.selection_active;
    out->prime_pending = 0; /* retained debug ABI; no synthetic DATA events */
    out->data_deliveries = ciap.data_deliveries;
    out->locator_deliveries = ciap.locator_deliveries;
    out->held_ticks = ciap.held_ticks;
}

uint32_t cdic_read(uint32_t address, int size) {
    uint32_t offset = address - CDI_CDIC_BASE;
    uint32_t result = 0;
    int i;

    ensure_initialized();
    if (size <= 0 || offset >= CIAP_REGISTER_BYTES ||
        (uint32_t)size > CIAP_REGISTER_BYTES - offset)
        unknown_read(address, offset, size);

    for (i = 0; i < size; i++) {
        uint32_t byte_offset = offset + (uint32_t)i;
        uint8_t value = ciap.memory[byte_offset];
        /* Until a stream is armed, the firmware presents its $0009 idle
         * work-ready status word in the ISR read port on top of the event
         * latch. The latch itself stays genuine: it alone drives the
         * interrupt line and is what acknowledge-on-read consumes. */
        if (ciap.firmware_idle && byte_offset == CIAP_ISR)
            value |= (uint8_t)(CIAP_ISR_FIRMWARE_IDLE >> 8);
        if (ciap.firmware_idle && byte_offset == CIAP_ISR + 1u)
            value |= (uint8_t)CIAP_ISR_FIRMWARE_IDLE;
        result = (result << 8) | value;
    }
    record_access(offset, result, size, 0);

    /* ISR is acknowledge-on-read. The production driver uses aligned words;
     * treating any access covering both bytes the same keeps long reads sane. */
    if (offset <= CIAP_ISR && offset + (uint32_t)size >= CIAP_ISR + 2u) {
        store_word(CIAP_ISR, 0);
        clear_interrupt_line();
    }
    return result;
}

static void reset_data_path(void) {
    ciap.data_running = 0;
    ciap.data_path_armed = 0;
    /* $0100 stop/flush deactivates selection until the next ASEL ($0008):
     * the boot-module loads re-arm with a bare $0100+$00C4 (no ASEL) and
     * expect an unfiltered stream, while the play arm ($42BF74) explicitly
     * re-issues ASEL before its $7000/$0044 locator start. Persisting the
     * selection across the flush stalls the very first boot-module read
     * behind a held sector its mask never matches. */
    ciap.selection_active = 0;
    ciap.selection_header_pending = 0;
    ciap.q_reporting = 0;
    ciap.next_data_buffer = 0;
    ciap.sector_elapsed_ns = 0;
    /* CCR resets the CD decoder, not the independent audio processor.
     * CD-RTOS resets/positions the decoder while a memory sound is playing,
     * then stops that sound by waiting for its next PCL interrupt ($429FE0).
     * Cancelling ADPCM here makes that wait time out and leaves audio record
     * CILs busy. APCR $6 is the separate audio reset. */
    store_word(CIAP_BMAN, (uint16_t)(read_word(CIAP_BMAN) & 3u));
    store_word(CIAP_ISR, (uint16_t)(read_word(CIAP_ISR) & 0x000Au));
    clear_interrupt_line();
    assert_interrupt_line();
}

static void start_audio_buffer(void) {
    unsigned buffer = ciap.audio_buffer;
    if (ciap.audio_select_pending && (read_word(CIAP_BMAN) & 3u)) {
        if (!(read_word(CIAP_BMAN) & (1u << buffer))) buffer ^= 1u;
        ciap.audio_buffer = (uint8_t)buffer;
        ciap.audio_select_pending = 0;
    }
    uint16_t bit = (uint16_t)(1u << buffer);
    if (!ciap.audio_playing || ciap.audio_active || !(read_word(CIAP_BMAN) & bit))
        return;
    uint16_t format = ciap.audio_format[buffer];
    uint8_t coding = (uint8_t)(((format & 0x2000u) ? 0x10u : 0u) |
                              ((format & 0x1000u) ? 0x04u : 0u) |
                              ((format & 0x0800u) ? 0x01u : 0u));
    uint32_t frames = cdi_audio_decode_groups(ciap.memory + buffer * 0x900u, coding);
    ciap.audio_remaining_ns = ((uint64_t)frames * 1000000000ull) /
                             CDI_AUDIO_OUTPUT_RATE;
    ciap.audio_duration_ns = ciap.audio_remaining_ns;
    ciap.audio_active = 1;
    store_word(CIAP_AP_POINTER, (uint16_t)(buffer * 0x480u));
    store_word(CIAP_ASTAT, (uint16_t)(read_word(CIAP_ASTAT) | 0x0040u));
}

static void request_ap_completion(void) {
    ciap.ap_completion_pending = 1;
    ciap.ap_completion_delay_ns = 20000u;
}

static void handle_register_write(uint32_t offset, uint16_t value) {
    switch (offset) {
    case CIAP_IER:
        store_word(offset, value);
        assert_interrupt_line();
        break;
    case CIAP_BMAN:
    {
        /* DATA/subcode writes release serviced host buffers. The ROM writes
         * an entire pair mask even when only one buffer is ready. ADPCM
         * ownership is a separate XOR handoff below. This is the firmware's
         * host contract, inferred from $4292F0/$429346; microcode-level BMAN
         * toggling still needs independent hardware validation. */
        uint16_t next = (uint16_t)(read_word(offset) &
                                   ~(value & (CIAP_BMAN_DATA0 |
                                              CIAP_BMAN_DATA1 |
                                              CIAP_BMAN_ACK_CLEAR)));
        store_word(offset, next);
        /* The host hands ADPCM buffers to the audio processor by toggling
         * bits 0/1. Capture each buffer's coding before A_SHDW is reused. */
        next ^= value & 3u;
        store_word(offset, next);
        for (unsigned buffer = 0; buffer < 2; buffer++)
            if ((value & next) & (1u << buffer))
                ciap.audio_format[buffer] = read_word(CIAP_A_SHDW);
        start_audio_buffer();
        break;
    }
    case CIAP_CCR:
        store_word(offset, value);
        if (value == CIAP_CCR_RESET) reset_data_path();
        else if (value == CIAP_CCR_MEMORY_PLAY) {
            /* The ROM parks sector decode before memory-backed PCM play
             * ($429D66). Unlike RESET, this preserves buffer ownership. */
            ciap.data_running = 0;
        }
        else if (value == CIAP_CCR_ASEL) {
            ciap.selection_active = 1;
            ciap.selection_header_pending = 1;
        }
        else if (value == CIAP_CCR_STARTD ||
                 value == CIAP_CCR_STARTQ_RECORD ||
                 value == CIAP_CCR_STARTQ_PLAY ||
                 value == CIAP_CCR_STARTQ_RETRY) {
            ciap.data_path_armed = 1;
            ciap.firmware_idle = 0;
            ciap.data_running = 1;
            ciap.sector_elapsed_ns = 0;
            /* Locator reporting is a mode armed explicitly by the $0010/
             * $0044/$0094 starts; a plain STARTD stream (boot module loads)
             * never acknowledges the locator buffers. */
            ciap.q_reporting = (value != CIAP_CCR_STARTD);
        }
        break;
    case CIAP_APCR:
        store_word(offset, value);
        /* Starting an audio-processor operation is not itself a completion.
         * CD-RTOS follows the setup command with INTNOW (or FINISH at EOR)
         * once the current buffer is ready, then acknowledges the resulting
         * AP interrupt with ACK.  Completing the setup command synchronously
         * re-enters the driver before it can publish its new state. */
        if (value == CIAP_APCR_ACK) {
            store_word(CIAP_ASTAT,
                       (uint16_t)(read_word(CIAP_ASTAT) & ~0x0080u));
            store_word(CIAP_ISR,
                       (uint16_t)(read_word(CIAP_ISR) & ~0x0008u));
            clear_interrupt_line();
            assert_interrupt_line();
        }
        else if (value == 0x0006u) {
            ciap.ap_completion_pending = 0;
            ciap.ap_completion_delay_ns = 0;
            ciap.audio_playing = 0;
            ciap.audio_active = 0;
            ciap.audio_finish_requested = 0;
            ciap.audio_remaining_ns = 0;
            ciap.audio_duration_ns = 0;
            ciap.audio_select_pending = 0;
            store_word(CIAP_AP_POINTER, 0);
            store_word(CIAP_BMAN, (uint16_t)(read_word(CIAP_BMAN) & ~3u));
            store_word(CIAP_ASTAT, (uint16_t)(read_word(CIAP_ASTAT) & ~0x00C0u));
            store_word(CIAP_ISR, (uint16_t)(read_word(CIAP_ISR) & ~0x000Au));
            clear_interrupt_line();
            assert_interrupt_line();
        }
        else if (value == 0x0040u || value == 0x0140u) {
            /* PLAY0 starts the buffer consumer. It is not an immediate
             * completion: the ROM refills through the PCL buffer interrupt. */
            ciap.audio_playing = 1;
            /* Plain PLAY0 is also used to replace a sound while the record
             * engine runs ($429C98). Keep an active buffer's progress; when
             * idle, the first handed-off buffer starts the new consumer. */
            if (value == 0x0140u) {
                ciap.audio_buffer = 0;
                ciap.audio_select_pending = 0;
            } else if (!ciap.audio_active) ciap.audio_select_pending = 1;
            ciap.audio_finish_requested = 0;
            start_audio_buffer();
        }
        else if (value == CIAP_APCR_FINISH) {
            ciap.audio_finish_requested = 1;
            if (!ciap.audio_active && !(read_word(CIAP_BMAN) & 3u)) {
                ciap.audio_playing = 0;
                ciap.audio_finish_requested = 0;
                request_ap_completion();
            }
        }
        else if (value == CIAP_APCR_INTNOW ||
                 (value & CIAP_APCR_IE)) {
            /* Bit 8 asks the audio processor to interrupt when the command
             * completes. The CD driver's transport ops write $142/$140/$101
             * and then park in status 8 until ISR bit 3 delivers the
             * completion ($42940C -> $4294A0); without it the SS_Play state
             * machine never reaches status 5 and the PCL processor stays
             * gated off. */
            /* The audio processor executes the command and interrupts
             * LATER. Completing synchronously inside this write re-enters
             * the driver's AP dispatch before the caller reaches its
             * BSET #6 notify arm four instructions after the APCR store
             * ($428836/$42883A), stranding the client wake forever. */
            /* Long enough to let the caller finish arming its notify state
             * (a handful of instructions), short enough that the ROM's
             * bounded completion polls (e.g. the 400-iteration wait at
             * $429214) still observe it. */
            request_ap_completion();
        }
        break;
    case CIAP_DMACTL:
        store_word(offset, value);
        if (value & 0x4000u) periph_ciap_dma_request(value);
        break;
    case CIAP_ID:
        /* Read-only silicon identification. */
        break;
    case CIAP_DLOAD:
        store_word(offset, value);
        /* Entering firmware-download mode restarts the CIAP execution core.
         * The external drive position survives a main-CPU reset, but the
         * freshly uploaded firmware must expose its idle work-ready state
         * again before CD-RTOS arms another data stream. */
        if (value & 1u) {
            reset_data_path();
            ciap.ap_completion_pending = 0;
            ciap.audio_playing = ciap.audio_active = 0;
            ciap.audio_finish_requested = ciap.audio_select_pending = 0;
            ciap.audio_remaining_ns = ciap.audio_duration_ns = 0;
            store_word(CIAP_BMAN, 0);
            store_word(CIAP_ISR, 0);
            store_word(CIAP_ASTAT, 0x0400u);
            store_word(CIAP_AP_POINTER, 0);
            clear_interrupt_line();
            ciap.firmware_idle = 1;
        }
        break;
    default:
        store_word(offset, value);
        break;
    }
}

void cdic_write(uint32_t address, uint32_t value, int size) {
    uint32_t offset = address - CDI_CDIC_BASE;
    int i;

    ensure_initialized();
    record_access(offset, value, size, 1);

    /* The ROM uploads firmware through the full write-only program aperture. */
    if (size <= 0 || offset >= CIAP_APERTURE_BYTES ||
        (uint32_t)size > CIAP_APERTURE_BYTES - offset)
        return;

    if (size == 2 && !(offset & 1u) && offset >= CIAP_IER &&
        offset < CIAP_REGISTER_BYTES) {
        handle_register_write(offset, (uint16_t)value);
        return;
    }
    for (i = 0; i < size; i++) {
        unsigned shift = 8u * (unsigned)(size - i - 1);
        ciap.memory[offset + (uint32_t)i] = (uint8_t)(value >> shift);
    }
}

/* Drive pause preserves the decoder arm. Decoder RESET cancels it: a delayed
 * IKAT resume must not restart delivery into handlers the ROM has torn down.
 * A new operation explicitly arms the decoder through CCR. */
void cdic_transport_pause(void) {
    ensure_initialized();
    ciap.data_running = 0;
}

void cdic_transport_resume(void) {
    ensure_initialized();
    /* An audio play (E0 positioning) keeps the data path parked; resuming
     * it would flood DATA interrupts into a driver phase that expects
     * none. */
    if (ciap.data_path_armed && ciap.drive_positioned &&
        !ciap.audio_positioned) {
        ciap.data_running = 1;
        ciap.sector_elapsed_ns = 0;
        /* No re-announce of an un-acked buffer here.  A stray delivery no
         * longer parks the transport (double-buffered ownership: only
         * both-buffers-owned holds the drive), so the next real sector's
         * DATA edge wakes the driver by itself — the failure the old
         * re-announce papered over (the real-time-pace game-load wedge).
         * Re-announcing was also indistinguishable from a fresh delivery
         * while the driver was MID-SERVICE on that same buffer: it made
         * the ROM consume the sector twice, running its per-op DMA
         * destination chain one sector ahead into a NULL pointer (payload
         * DMA over the vector table at $0). */
    }
}

void cdic_set_drive_position(uint32_t lba, int audio) {
    ensure_initialized();
    ciap.drive_lba = lba;
    ciap.drive_positioned = 1;
    ciap.audio_positioned = (uint8_t)(audio != 0);
    /* STARTA is intentionally not decoded yet; retain the mode distinction so
     * the audio path can attach here without changing the IKAT contract. */
    if (audio) ciap.data_running = 0;
}

static int sector_selected(const uint8_t sector[CIAP_SECTOR_BYTES]) {
    unsigned file;
    unsigned channel;
    uint32_t mask;

    if (!ciap.selection_active || sector[3] != 2u) return 1;
    file = sector[4];
    channel = sector[5] & 31u;
    /* A Mode-2 subheader is recorded twice.  CIAP rejects a sector whose
     * selection fields do not agree; passing it onward would turn a damaged
     * header into arbitrary file/channel routing. */
    if (sector[4] != sector[8] || sector[5] != sector[9] ||
        sector[6] != sector[10] || sector[7] != sector[11])
        return 0;
    /* FILE zero disables file-number matching. Hotel Mario uses this wildcard
     * for compound realtime streams whose map is file 0 while executable/data
     * extents are carried as file 1 on a selected channel. */
    {
        unsigned selected_file = read_word(CIAP_FILE) & 0xFFu;
        if (selected_file && file != selected_file) return 0;
    }
    /* Selection controls payload delivery. File-wide trigger/EOF events are
     * handled separately without transferring an unselected payload. */
    mask = (uint32_t)read_word(CIAP_TCM1) |
           ((uint32_t)read_word(CIAP_ACM2) << 16);
    return (mask & (1u << channel)) != 0;
}

static uint8_t to_bcd(uint32_t v) {
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/* CIAP presents the ten Q bytes without CRC. ADR=1 and CONTROL bit 6 marks
 * this data track (the CD-DA path rejects it at ROM $429A84). Position is
 * absolute BCD MSF in bytes 7-9. EOF is a mainchannel event, not a Q flag. */
static void write_q_locator(unsigned offset, uint32_t lba) {
    const uint32_t real = lba + 150u;
    uint8_t q[10];

    q[0] = 0x41u;
    q[1] = 0x00u;
    q[2] = 0x01u;
    q[3] = to_bcd(real / 4500u);
    q[4] = to_bcd((real / 75u) % 60u);
    q[5] = to_bcd(real % 75u);
    q[6] = 0;
    q[7] = q[3];
    q[8] = q[4];
    q[9] = q[5];
    memcpy(ciap.memory + offset, q, sizeof q);
}

static void deliver_one_sector(void) {
    uint8_t sector[CIAP_SECTOR_BYTES];
    uint16_t bman;
    unsigned buffer;
    unsigned bit;
    unsigned offset;

    if (!ciap.drive_positioned || !cdi_media_present()) return;
    uint32_t lba = ciap.drive_lba;
    if (!cdi_media_read_sector_body(lba, sector)) {
        fprintf(stderr,
                "[ciap] media read failed at LBA %u; data path stopped\n",
                lba);
        debug_dump_fault_trail("CIAP media read failure");
        ciap.data_running = 0;
        return;
    }
    ciap.last_lba = lba;
    ciap.last_file = sector[4];
    ciap.last_channel = sector[5];
    ciap.last_submode = sector[6];
    ciap.last_coding = sector[7];
    ciap.last_selected = (uint8_t)sector_selected(sector);
    /* The original play arm installs a one-buffer discard callback after
     * ASEL ($4286D0). The next physical sector must provide that boundary
     * header even when its file/channel is filtered; otherwise the callback
     * consumes the first selected payload instead. This is inferred from
     * the firmware handshake. It never fabricates or repeats a sector. */
    int selection_header = ciap.selection_header_pending;
    ciap.selection_header_pending = 0;
    /* Trigger/EOF are file-wide events, independent of payload channels.
     * The original intro stream puts scene triggers on channel 0 while its
     * pictures/audio select channel 15. ROM $428C1C consumes ISR9/10 without
     * a DATA buffer; delivering channel 0 payload would corrupt the video.
     * Reject damaged duplicate subheaders before interpreting these bits. */
    if (sector[3] == 2u && !memcmp(sector + 4, sector + 8, 4) &&
        (!ciap.selection_active || !read_word(CIAP_FILE) ||
         (read_word(CIAP_FILE) & 0xFFu) == sector[4]) &&
        (sector[6] & (CD_I_SUBMODE_TRIGGER | CD_I_SUBMODE_EOF))) {
        uint16_t events = (sector[6] & CD_I_SUBMODE_TRIGGER)
                            ? CIAP_ISR_STREAM_TRIGGER : 0;
        if (sector[6] & CD_I_SUBMODE_EOF) events |= CIAP_ISR_FILE_END;
        store_word(CIAP_ISR,
                   (uint16_t)(read_word(CIAP_ISR) | events));
        assert_interrupt_line();
    }
    /* Drive-position locators are selection-independent: the drivers'
     * seek-confirm ($428D8A) must see the sectors approaching a target even
     * when no channel selects them, so every sector passing the head
     * reports, alternating the locator pair on its own. */
    if (ciap.q_reporting) {
        ciap.locator_deliveries++;
        unsigned qi = ciap.next_q_buffer;
        uint16_t qbit = qi ? CIAP_BMAN_Q1 : CIAP_BMAN_Q0;
        uint16_t qbman = read_word(CIAP_BMAN);
        ciap.next_q_buffer = (uint8_t)(qi ^ 1u);
        write_q_locator(qi ? CIAP_QBUF1 : CIAP_QBUF0, lba);
        /* ISR bit 11 reports an invalid Q locator, not host ownership.
         * These locators are synthesized from a valid disc position. */
        store_word(CIAP_BMAN, (uint16_t)(qbman | qbit));
        store_word(CIAP_ISR,
                   (uint16_t)(read_word(CIAP_ISR) | CIAP_ISR_QREADY));
        assert_interrupt_line();
    }
    /* Payload selection must not freeze the file position between selected
     * channels. The real CDFM SS.Pos reads its driver-maintained offset at
     * $41553A; $428AA2 advances it from the host sector header. Inferred from
     * that firmware ABI, regular sectors of the selected file retain a
     * header-only host notification when their payload channel is filtered.
     * The ROM's own channel test at $428A72 prevents payload DMA. Record
     * boundaries stay selected-channel events: never feed an unselected EOR
     * to the record engine. Unselected trigger/EOF already use ISR9/10 above.
     * No selection command may invent a delivery without a real sector. */
    int metadata_only = !ciap.last_selected && sector[3] == 2u &&
                        !memcmp(sector + 4, sector + 8, 4) &&
                        (selection_header ||
                         (!(sector[6] & (CD_I_SUBMODE_EOR | CD_I_SUBMODE_TRIGGER | CD_I_SUBMODE_EOF)) &&
                          (!read_word(CIAP_FILE) ||
                           (read_word(CIAP_FILE) & 0xFFu) == sector[4])));
    if (!ciap.last_selected && !metadata_only) {
        ciap.drive_lba++;
        return;
    }
    /* TACS routes ADPCM to the audio processor. The selected sector's
     * header still advances host record accounting ($428AA2); the ROM
     * excludes TACS audio from payload DMA at $428A84. Suppressing the
     * header notification until EOR leaves SS.Pos frozen throughout music. */
    int direct_audio = ciap.last_selected && (sector[6] & CD_I_SUBMODE_AUDIO) && sector[5] < 16u &&
                       (read_word(CIAP_TACS) & (1u << sector[5]));
    if (direct_audio) {
        cdi_audio_decode_sector(sector);
    }

    ciap.drive_lba++;

    /* CIAP has no drive-control interface. A full host buffer pair inhibits
     * payload delivery, but cannot stop physical sectors or position reports.
     * Keep owned bytes intact until the host releases them. */
    if (data_buffers_full()) {
        ciap.held_ticks++;
        return;
    }

    buffer = ciap.next_data_buffer;
    /* Never overwrite a buffer the host still owns: fill the free one. */
    if (read_word(CIAP_BMAN) &
        (buffer ? CIAP_BMAN_DATA1 : CIAP_BMAN_DATA0))
        buffer ^= 1u;
    bit = buffer ? CIAP_BMAN_DATA1 : CIAP_BMAN_DATA0;
    offset = buffer ? CIAP_DATA1 : CIAP_DATA0;
    if (direct_audio || metadata_only) {
        memcpy(ciap.memory + offset, sector, 12u);
        memset(ciap.memory + offset + 12u, 0, sizeof sector - 12u);
    } else memcpy(ciap.memory + offset, sector, sizeof sector);
    ciap.data_deliveries++;
    /* Every host notification owns a real header buffer. The ROM's picker
     * reads its BMAN bit and the CIAP alternates fill buffers itself. */
    bman = read_word(CIAP_BMAN);
    store_word(CIAP_BMAN, (uint16_t)(bman | bit));
    ciap.next_data_buffer = (uint8_t)(buffer ^ 1u);
    store_word(CIAP_ISR, (uint16_t)(read_word(CIAP_ISR) | CIAP_ISR_DATA));
    assert_interrupt_line();
}

void cdic_increment_time(double nanoseconds) {
    const uint64_t sector_period_ns = 1000000000ull / 75ull;
    ensure_initialized();
    if (ciap.ap_completion_pending) {
        uint64_t step = (uint64_t)nanoseconds;
        if (step >= ciap.ap_completion_delay_ns) {
            ciap.ap_completion_pending = 0;
            ciap.ap_completion_delay_ns = 0;
            store_word(CIAP_ASTAT,
                       (uint16_t)(read_word(CIAP_ASTAT) | 0x0080u));
            store_word(CIAP_ISR,
                       (uint16_t)(read_word(CIAP_ISR) | 0x0008u));
            assert_interrupt_line();
        } else {
            ciap.ap_completion_delay_ns -= step;
        }
    }
    uint64_t audio_step = (uint64_t)nanoseconds;
    while (ciap.audio_active && audio_step >= ciap.audio_remaining_ns) {
        audio_step -= ciap.audio_remaining_ns;
        uint16_t bit = (uint16_t)(1u << ciap.audio_buffer);
        store_word(CIAP_BMAN, (uint16_t)(read_word(CIAP_BMAN) & ~bit));
        ciap.audio_active = 0;
        ciap.audio_remaining_ns = 0;
        ciap.audio_buffer ^= 1u;
        store_word(CIAP_AP_POINTER, (uint16_t)(ciap.audio_buffer * 0x480u));
        /* Bit 1 wakes the PCL refill path ($429E0C). Bit 13 is a different
         * condition: it makes that handler probe/acknowledge the ADPCM pair
         * and take its error path when neither buffer is owned ($429EFE).
         * An ordinary consumed-buffer notification must not set it. */
        store_word(CIAP_ISR, (uint16_t)(read_word(CIAP_ISR) | 0x0002u));
        assert_interrupt_line();
        start_audio_buffer();
        if (!ciap.audio_active) {
            store_word(CIAP_ASTAT, (uint16_t)(read_word(CIAP_ASTAT) & ~0x0040u));
            if (ciap.audio_finish_requested) {
                ciap.audio_playing = 0;
                ciap.audio_finish_requested = 0;
                request_ap_completion();
            }
        }
    }
    if (ciap.audio_active) {
        ciap.audio_remaining_ns -= audio_step;
        uint64_t elapsed = ciap.audio_duration_ns - ciap.audio_remaining_ns;
        store_word(CIAP_AP_POINTER, (uint16_t)(ciap.audio_buffer * 0x480u +
                   elapsed * 0x480u / ciap.audio_duration_ns));
    }
    if (!ciap.data_running) return;
    ciap.sector_elapsed_ns += (uint64_t)nanoseconds;
    while (ciap.sector_elapsed_ns >= sector_period_ns) {
        ciap.sector_elapsed_ns -= sector_period_ns;
        deliver_one_sector();
        if (!ciap.data_running) break;
    }
}

uint16_t cdic_dma_pull_word(void) {
    uint16_t control;
    unsigned word_offset;
    ensure_initialized();
    control = read_word(CIAP_DMACTL);
    word_offset = control & 0x1FFFu;
    if (word_offset * 2u + 1u >= CIAP_APERTURE_BYTES) return 0;
    control = (uint16_t)((control & 0xE000u) |
                         ((word_offset + 1u) & 0x1FFFu));
    store_word(CIAP_DMACTL, control);
    return read_word(word_offset * 2u);
}

void cdic_dma_push_word(uint16_t value) {
    uint16_t control;
    unsigned word_offset;
    ensure_initialized();
    control = read_word(CIAP_DMACTL);
    word_offset = control & 0x1FFFu;
    if (word_offset * 2u + 1u < CIAP_APERTURE_BYTES)
        store_word(word_offset * 2u, value);
    control = (uint16_t)((control & 0xE000u) |
                         ((word_offset + 1u) & 0x1FFFu));
    store_word(CIAP_DMACTL, control);
}

void cdic_dma_complete(void) {
    ensure_initialized();
    store_word(CIAP_DMACTL,
               (uint16_t)(read_word(CIAP_DMACTL) & (uint16_t)~0x4000u));
}
