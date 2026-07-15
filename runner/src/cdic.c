/*
 * Mono-III/IV CIAP register window.
 *
 * This intentionally models only behavior established by the cdi490 CD-RTOS
 * probe and access traces: a fixed identification word, the polled MCD221 data
 * status value, byte-addressable register storage, and a write-only firmware
 * upload area beyond the readable register window.  Unknown readable offsets
 * fail loudly instead of inheriting behavior from an external emulator.
 */
#include "cdi_runtime.h"
#include "debug_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CIAP_DATA_STATUS = 0x2586u,
    CIAP_IDENTIFICATION = 0x25C4u,
    CIAP_REGISTER_BYTES = 0x2600u
};

enum {
    CIAP_EVENT_CAPACITY = 1u << 16,
    CIAP_EVENT_INDEX_MASK = CIAP_EVENT_CAPACITY - 1u
};

typedef struct {
    uint8_t registers[CIAP_REGISTER_BYTES];
    CdiCiapEvent events[CIAP_EVENT_CAPACITY];
    uint64_t event_count;
    int initialized;
} CiapDevice;

static CiapDevice ciap;

static void initialize_ciap(void) {
    memset(&ciap, 0, sizeof ciap);
    ciap.registers[CIAP_IDENTIFICATION] = 0xCD;
    ciap.registers[CIAP_IDENTIFICATION + 1] = 0x02;
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

static uint8_t read_register_byte(uint32_t offset) {
    /* CD-RTOS polls this word after each instruction-sized work unit.  The
     * observed ready value is $0009. */
    if (offset == CIAP_DATA_STATUS) return 0;
    if (offset == CIAP_DATA_STATUS + 1) return 9;
    return ciap.registers[offset];
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

    for (i = 0; i < size; i++)
        result = (result << 8) | read_register_byte(offset + (uint32_t)i);
    record_access(offset, result, size, 0);
    return result;
}

void cdic_write(uint32_t address, uint32_t value, int size) {
    uint32_t offset = address - CDI_CDIC_BASE;
    int i;

    ensure_initialized();
    record_access(offset, value, size, 1);

    /* The ROM uploads CIAP firmware through a larger write-only aperture. */
    if (size <= 0 || offset >= CIAP_REGISTER_BYTES ||
        (uint32_t)size > CIAP_REGISTER_BYTES - offset)
        return;

    for (i = 0; i < size; i++) {
        unsigned shift = 8u * (unsigned)(size - i - 1);
        ciap.registers[offset + (uint32_t)i] = (uint8_t)(value >> shift);
    }
}
