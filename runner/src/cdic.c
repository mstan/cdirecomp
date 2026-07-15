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
    CIAP_IER = 0x2584u,
    CIAP_ISR = 0x2586u,
    CIAP_TCM1 = 0x258Cu,
    CIAP_ACM2 = 0x2590u,
    CIAP_FILE = 0x2592u,
    CIAP_BMAN = 0x2594u,
    CIAP_CCR = 0x2596u,
    CIAP_APCR = 0x25A6u,
    CIAP_ASTAT = 0x25AAu,
    CIAP_ICR = 0x25C0u,
    CIAP_DMACTL = 0x25C2u,
    CIAP_ID = 0x25C4u,
    CIAP_DLOAD = 0x25FEu,
    CIAP_REGISTER_BYTES = 0x2600u,
    CIAP_APERTURE_BYTES = 0x4000u,
    CIAP_SECTOR_BYTES = 2340u,
    CIAP_ISR_DATA = 0x0001u,
    CIAP_BMAN_DATA0 = 0x0004u,
    CIAP_BMAN_DATA1 = 0x0008u,
    CIAP_CCR_ASEL = 0x0008u,
    CIAP_CCR_STARTD = 0x00C4u,
    CIAP_CCR_RESET = 0x0100u,
    CIAP_APCR_FINISH = 0x0020u,
    CIAP_APCR_INTNOW = 0x00A0u,
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
    uint32_t drive_lba;
    uint32_t last_lba;
    uint8_t last_file;
    uint8_t last_channel;
    uint8_t last_submode;
    uint8_t last_coding;
    uint8_t last_selected;
    uint8_t next_data_buffer;
    uint8_t drive_positioned;
    uint8_t data_running;
    uint8_t data_path_armed;
    uint8_t selection_prime_pending;
    uint8_t selection_active;
    uint8_t buffer_waiting_ack;
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
    *waiting_ack = ciap.buffer_waiting_ack;
}

static void unknown_read(uint32_t address, uint32_t offset, int size) {
    fprintf(stderr,
            "[ciap] read%d @ $%08X (offset $%X) outside readable registers\n",
            size * 8, address, offset);
    debug_dump_fault_trail("CIAP read outside readable registers");
    if (g_hold_on_fault) cdi_fault_hold();
    abort();
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
        /* The initialized CIAP firmware presents $0009 as its idle work-ready
         * word before the first stream. This is distinct from the live ISR
         * latch used after STARTD and is required by the cdi490 INIT worker. */
        if (!ciap.data_path_armed && byte_offset == CIAP_ISR) value = 0;
        if (!ciap.data_path_armed && byte_offset == CIAP_ISR + 1u) value = 9;
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
    ciap.selection_active = 0;
    ciap.next_data_buffer = 0;
    ciap.buffer_waiting_ack = 0;
    ciap.selection_prime_pending = 0;
    ciap.sector_elapsed_ns = 0;
    store_word(CIAP_BMAN, 0);
    store_word(CIAP_ISR, 0);
    store_word(CIAP_ASTAT, 0x0400u);
    clear_interrupt_line();
}

static void handle_register_write(uint32_t offset, uint16_t value) {
    switch (offset) {
    case CIAP_IER:
        store_word(offset, value);
        assert_interrupt_line();
        break;
    case CIAP_BMAN:
    {
        uint16_t next = (uint16_t)(read_word(offset) ^ value);
        store_word(offset, next);
        /* CD-RTOS acknowledges a consumed main-channel buffer by writing
         * DATA0|DATA1. The XOR transition is a one-hot ownership handoff:
         * 01 -> 10 or 10 -> 01. The selected bit names the buffer CIAP fills
         * on the next DATA interrupt; it is not a pair of independent FIFO
         * occupancy flags. */
        if ((next & (CIAP_BMAN_DATA0 | CIAP_BMAN_DATA1)) ==
            CIAP_BMAN_DATA0) {
            ciap.next_data_buffer = 0;
            ciap.buffer_waiting_ack = 0;
        }
        else if ((next & (CIAP_BMAN_DATA0 | CIAP_BMAN_DATA1)) ==
                 CIAP_BMAN_DATA1) {
            ciap.next_data_buffer = 1;
            ciap.buffer_waiting_ack = 0;
        }
        break;
    }
    case CIAP_CCR:
        store_word(offset, value);
        if (value == CIAP_CCR_RESET) reset_data_path();
        else if (value == CIAP_CCR_ASEL) {
            uint32_t mask = (uint32_t)read_word(CIAP_TCM1) |
                            ((uint32_t)read_word(CIAP_ACM2) << 16);
            ciap.selection_active = 1;
            if (ciap.data_running && mask != 0u &&
                !(mask & (mask - 1u)))
                ciap.selection_prime_pending = 1;
        }
        else if (value == CIAP_CCR_STARTD) {
            ciap.data_path_armed = 1;
            ciap.data_running = 1;
            ciap.sector_elapsed_ns = 0;
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
        else if (value == CIAP_APCR_INTNOW || value == CIAP_APCR_FINISH) {
            store_word(CIAP_ASTAT,
                       (uint16_t)(read_word(CIAP_ASTAT) | 0x0080u));
            store_word(CIAP_ISR,
                       (uint16_t)(read_word(CIAP_ISR) | 0x0008u));
            assert_interrupt_line();
        }
        break;
    case CIAP_TCM1:
    case CIAP_ACM2:
    {
        uint32_t old_mask = (uint32_t)read_word(CIAP_TCM1) |
                            ((uint32_t)read_word(CIAP_ACM2) << 16);
        uint32_t new_mask;
        store_word(offset, value);
        new_mask = (uint32_t)read_word(CIAP_TCM1) |
                   ((uint32_t)read_word(CIAP_ACM2) << 16);
        if (ciap.data_running && old_mask != new_mask && new_mask != 0u &&
            !(new_mask & (new_mask - 1u)))
            ciap.selection_prime_pending = 1;
        break;
    }
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
            ciap.data_path_armed = 0;
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

void cdic_set_drive_position(uint32_t lba, int audio) {
    ensure_initialized();
    ciap.drive_lba = lba;
    ciap.drive_positioned = 1;
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
    /* Record markers remain part of their file/channel stream.  Bypassing the
     * channel mask for EOF/EOR/trigger sectors feeds unrelated, commonly
     * zero-filled record tails into the selected realtime decoder. */
    mask = (uint32_t)read_word(CIAP_TCM1) |
           ((uint32_t)read_word(CIAP_ACM2) << 16);
    return (mask & (1u << channel)) != 0;
}

static void deliver_one_sector(void) {
    uint8_t sector[CIAP_SECTOR_BYTES];
    uint16_t bman;
    unsigned buffer;
    unsigned bit;
    unsigned offset;

    if (!ciap.drive_positioned || !cdi_media_present()) return;
    /* Our current CIAP model exposes one completed sector at a time.  Hold
     * the drive position until the host returns that buffer; advancing here
     * loses the next selected sector whenever software services DATA later
     * than the 75 Hz sector interval. */
    if (ciap.buffer_waiting_ack) return;

    uint32_t lba = ciap.drive_lba;
    if (!cdi_media_read_sector_body(lba, sector)) {
        ciap.data_running = 0;
        return;
    }
    ciap.last_lba = lba;
    ciap.last_file = sector[4];
    ciap.last_channel = sector[5];
    ciap.last_submode = sector[6];
    ciap.last_coding = sector[7];
    ciap.last_selected = (uint8_t)sector_selected(sector);
    if (!ciap.last_selected) {
        ciap.drive_lba++;
        return;
    }
    /* Audio sectors are consumed by the CIAP's audio processor and never take
     * ownership of a host data buffer. Treating them as DATA stalls the
     * realtime stream behind BMAN and prevents the application handoff. */
    if (sector[6] & CD_I_SUBMODE_AUDIO) {
        ciap.drive_lba++;
        cdi_audio_decode_sector(sector);
        return;
    }

    ciap.drive_lba++;

    buffer = ciap.next_data_buffer;
    bit = buffer ? CIAP_BMAN_DATA1 : CIAP_BMAN_DATA0;
    offset = buffer ? CIAP_DATA1 : CIAP_DATA0;
    memcpy(ciap.memory + offset, sector, sizeof sector);
    bman = read_word(CIAP_BMAN);
    if (!(bman & (CIAP_BMAN_DATA0 | CIAP_BMAN_DATA1)))
        store_word(CIAP_BMAN, (uint16_t)(bman | bit));
    ciap.buffer_waiting_ack = 1;
    store_word(CIAP_ISR, (uint16_t)(read_word(CIAP_ISR) | CIAP_ISR_DATA));
    assert_interrupt_line();
}

void cdic_increment_time(double nanoseconds) {
    const uint64_t sector_period_ns = 1000000000ull / 75ull;
    ensure_initialized();
    if (!ciap.data_running) return;
    ciap.sector_elapsed_ns += (uint64_t)nanoseconds;
    while (ciap.sector_elapsed_ns >= sector_period_ns) {
        ciap.sector_elapsed_ns -= sector_period_ns;
        if (ciap.selection_prime_pending && !ciap.buffer_waiting_ack) {
            ciap.selection_prime_pending = 0;
            store_word(CIAP_ISR,
                       (uint16_t)(read_word(CIAP_ISR) | CIAP_ISR_DATA));
            assert_interrupt_line();
            break;
        }
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
