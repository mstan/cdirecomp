/*
 * IKAT serial gate used by Mono-III/IV CD-i players.
 *
 * Protocol values come from the public mc6805ikat register notes and from
 * black-box traces of the cdi490 CD-RTOS drivers.  The implementation is a
 * four-channel message device: command bytes enter channel FIFOs, responses
 * leave independent FIFOs, and each channel owns one interrupt-status bit.
 */
#include "cdi_runtime.h"
#include "cdi_media.h"
#include "debug_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHANNEL_A, CHANNEL_B, CHANNEL_C, CHANNEL_D, CHANNEL_COUNT };
enum {
    REG_INPUT_A,
    REG_INPUT_B,
    REG_INPUT_C,
    REG_INPUT_D,
    REG_OUTPUT_A,
    REG_OUTPUT_B,
    REG_OUTPUT_C,
    REG_OUTPUT_D,
    REG_STATUS_A,
    REG_STATUS_B,
    REG_STATUS_C,
    REG_STATUS_D,
    REG_INTERRUPT_STATUS,
    REG_INTERRUPT_MASK,
    REG_MODE,
    IKAT_REGISTER_COUNT
};

enum {
    CHANNEL_BUFFER_BYTES = 8,
    IKAT_EVENT_CAPACITY = 256,
    INPUT_SCHEDULE_CAPACITY = 64
};

typedef struct {
    uint8_t command[CHANNEL_BUFFER_BYTES];
    uint8_t response[CHANNEL_BUFFER_BYTES];
    uint8_t deferred[CHANNEL_BUFFER_BYTES];
    uint8_t command_length;
    uint8_t response_length;
    uint8_t response_position;
    uint8_t deferred_length;
    uint8_t deferred_pending;
    uint64_t deferred_frame;
} IkatChannel;

typedef struct {
    uint8_t registers[IKAT_REGISTER_COUNT];
    IkatChannel channel[CHANNEL_COUNT];
    uint64_t media_generation;
    uint64_t pointer_elapsed_ns;
    uint32_t previous_input;
    int pointer_repeat_count;
    int initialized;
} IkatDevice;

typedef struct {
    uint64_t frame;
    uint32_t mask;
} InputTransition;

static IkatDevice ikat;
static InputTransition input_schedule[INPUT_SCHEDULE_CAPACITY];
static int input_schedule_count;
static int input_schedule_position;
static CdiIkatEvent event_ring[IKAT_EVENT_CAPACITY];
static uint64_t event_count;

static const uint8_t interrupt_bit[CHANNEL_COUNT] = { 0x02, 0x08, 0x20, 0x80 };
static const uint8_t reply_f0[] = { 0xA5, 0xF0, 0x7F };
static const uint8_t reply_f3[] = { 0xA5, 0xF3, 'M', 0x00 };
static const uint8_t reply_f4[] = { 0xA5, 0xF4, 0x00 };
static const uint8_t reply_f7[] = { 0xA5, 0xF7, 0x00 };
static const uint8_t reply_f8[] = { 0xA5, 0xF8, 0x00 };
static const uint8_t reply_b1[] = { 0xB1, 0x00, 0x02, 0x00 };
static const uint8_t reply_b2[] = { 0xB2, 0x20, 0x00, 0x10 };

static void record_event(uint8_t type, int channel,
                         const uint8_t *data, unsigned length) {
    CdiIkatEvent *event = &event_ring[event_count % IKAT_EVENT_CAPACITY];
    memset(event, 0, sizeof *event);
    event->seq = event_count++;
    event->trace_seq = debug_trace_sequence();
    event->frame = g_frame_count;
    event->cycles = g_total_cycles;
    event->pc = g_cpu.PC;
    event->type = type;
    event->channel = (uint8_t)channel;
    if (length > sizeof event->data) length = sizeof event->data;
    event->length = (uint8_t)length;
    if (data && length) memcpy(event->data, data, length);
}

int slave_debug_events(CdiIkatEvent *out, int capacity, uint64_t *total) {
    uint64_t end = event_count;
    uint64_t begin = end > IKAT_EVENT_CAPACITY ? end - IKAT_EVENT_CAPACITY : 0;
    int count = 0;
    if (capacity >= 0 && (uint64_t)capacity < end - begin)
        begin = end - (uint64_t)capacity;
    while (begin < end && count < capacity) {
        out[count++] = event_ring[begin % IKAT_EVENT_CAPACITY];
        begin++;
    }
    if (total) *total = end;
    return count;
}

static int parse_transition(char **cursor, InputTransition *transition,
                            uint64_t previous, int have_previous) {
    char *end;
    unsigned long long frame;
    unsigned long mask;

    if (**cursor == '-') return 0;
    errno = 0;
    frame = strtoull(*cursor, &end, 0);
    if (errno == ERANGE || end == *cursor || *end != ':') return 0;
    *cursor = end + 1;
    if (**cursor == '-') return 0;
    errno = 0;
    mask = strtoul(*cursor, &end, 0);
    if (errno == ERANGE || end == *cursor || mask > 0x3Fu ||
        (*end != ',' && *end != '\0') ||
        (have_previous && (uint64_t)frame <= previous))
        return 0;

    transition->frame = (uint64_t)frame;
    transition->mask = (uint32_t)mask;
    *cursor = end;
    return 1;
}

int cdi_input_schedule_configure(const char *specification) {
    char *copy;
    char *cursor;
    uint64_t previous = 0;
    int have_previous = 0;

    input_schedule_count = 0;
    input_schedule_position = 0;
    if (!specification || !*specification) return 0;

    copy = (char *)malloc(strlen(specification) + 1);
    if (!copy) return 0;
    strcpy(copy, specification);
    cursor = copy;

    while (*cursor && input_schedule_count < INPUT_SCHEDULE_CAPACITY) {
        InputTransition next;
        if (!parse_transition(&cursor, &next, previous, have_previous)) {
            input_schedule_count = 0;
            free(copy);
            return 0;
        }
        input_schedule[input_schedule_count++] = next;
        previous = next.frame;
        have_previous = 1;
        if (*cursor == ',') {
            cursor++;
            if (!*cursor) {
                input_schedule_count = 0;
                free(copy);
                return 0;
            }
        }
    }

    if (*cursor) input_schedule_count = 0;
    free(copy);
    return input_schedule_count != 0;
}

void cdi_input_schedule_advance(uint64_t completed_frame) {
    while (input_schedule_position < input_schedule_count &&
           input_schedule[input_schedule_position].frame <= completed_frame) {
        cdi_input_set(input_schedule[input_schedule_position].mask);
        input_schedule_position++;
    }
}

static int response_remaining(int channel) {
    IkatChannel *queue = &ikat.channel[channel];
    return (int)queue->response_length - (int)queue->response_position;
}

static void update_channel_status(int channel) {
    uint8_t *status = &ikat.registers[REG_STATUS_A + channel];
    if (response_remaining(channel))
        *status &= 0x01u;
    else
        *status |= 0x11u;
}

static void raise_channel_interrupt(int channel) {
    ikat.registers[REG_INTERRUPT_STATUS] |= interrupt_bit[channel];
    if (ikat.registers[REG_INTERRUPT_MASK] & interrupt_bit[channel]) {
        record_event(CDI_IKAT_IRQ, channel, NULL, 0);
        cdi_irq_raise(2);
    }
}

static void publish_response(int channel, const uint8_t *bytes, unsigned length) {
    IkatChannel *queue = &ikat.channel[channel];
    if (length > CHANNEL_BUFFER_BYTES) length = CHANNEL_BUFFER_BYTES;
    memcpy(queue->response, bytes, length);
    queue->response_position = 0;
    queue->response_length = (uint8_t)length;
    update_channel_status(channel);
    record_event(CDI_IKAT_RESPONSE, channel, bytes, length);
}

static void publish_and_interrupt(int channel, const uint8_t *bytes,
                                  unsigned length) {
    publish_response(channel, bytes, length);
    raise_channel_interrupt(channel);
}

static void defer_response(int channel, const uint8_t *bytes, unsigned length) {
    IkatChannel *queue = &ikat.channel[channel];
    if (length > CHANNEL_BUFFER_BYTES) length = CHANNEL_BUFFER_BYTES;
    memcpy(queue->deferred, bytes, length);
    queue->deferred_length = (uint8_t)length;
    queue->deferred_pending = 1;
    queue->deferred_frame = g_frame_count + 2;
}

static void make_disc_status(uint8_t status[4]) {
    status[0] = 0xB0;
    if (cdi_media_present()) {
        status[1] = 0x00;
        status[2] = 0x02;
        status[3] = 0x15;
    } else {
        status[1] = 0x02;
        status[2] = 0x10;
        status[3] = 0x00;
    }
}

typedef struct {
    uint8_t command;
    const uint8_t *reply;
    uint8_t reply_length;
} ImmediateCommand;

static void handle_channel_c(void) {
    static const ImmediateCommand commands[] = {
        { 0xF0, reply_f0, sizeof reply_f0 },
        { 0xF3, reply_f3, sizeof reply_f3 },
        { 0xF4, reply_f4, sizeof reply_f4 },
        { 0xF7, reply_f7, sizeof reply_f7 },
        { 0xF8, reply_f8, sizeof reply_f8 }
    };
    IkatChannel *channel = &ikat.channel[CHANNEL_C];
    unsigned i;

    record_event(CDI_IKAT_COMMAND, CHANNEL_C, channel->command,
                 channel->command_length);
    if (channel->command[0] == 0xF6) {
        const uint8_t video_reply[4] = { 0xA5, 0xF6, 0x01, 0xFF };
        channel->command_length = 0;
        publish_and_interrupt(CHANNEL_C, video_reply, sizeof video_reply);
        return;
    }
    for (i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        if (commands[i].command == channel->command[0]) {
            channel->command_length = 0;
            publish_and_interrupt(CHANNEL_C, commands[i].reply,
                                  commands[i].reply_length);
            return;
        }
    }
    channel->command_length = 0;
}

static int four_byte_command(uint8_t command) {
    return command == 0xA1 || command == 0xB0 || command == 0xB2 ||
           command == 0xE0 || command == 0xE1;
}

static void handle_channel_d(void) {
    IkatChannel *channel = &ikat.channel[CHANNEL_D];
    uint8_t command = channel->command[0];
    uint8_t disc_status[4];

    if (four_byte_command(command) && channel->command_length < 4) return;
    record_event(CDI_IKAT_COMMAND, CHANNEL_D, channel->command,
                 channel->command_length);

    switch (command) {
    case 0xA1:
    case 0xB0:
        make_disc_status(disc_status);
        defer_response(CHANNEL_D, disc_status, sizeof disc_status);
        break;
    case 0xB1:
        publish_and_interrupt(CHANNEL_D, reply_b1, sizeof reply_b1);
        break;
    case 0xB2:
        defer_response(CHANNEL_D, reply_b2, sizeof reply_b2);
        break;
    default:
        break;
    }
    channel->command_length = 0;
}

static void initialize_ikat(void) {
    int channel;
    uint64_t generation = cdi_media_generation();
    memset(&ikat, 0, sizeof ikat);
    for (channel = 0; channel < CHANNEL_COUNT; channel++)
        ikat.registers[REG_STATUS_A + channel] = 0x11;
    ikat.media_generation = generation;
    memset(event_ring, 0, sizeof event_ring);
    event_count = 0;
    ikat.initialized = 1;
}

static void ensure_initialized(void) {
    if (!ikat.initialized) initialize_ikat();
}

void slave_debug_state(uint8_t registers[15], uint8_t remaining[4],
                       double *pointer_time_ns, int *cursor_packets) {
    int channel;
    if (!ikat.initialized) {
        memset(registers, 0, 15);
        memset(remaining, 0, 4);
        *pointer_time_ns = 0.0;
        *cursor_packets = 0;
        return;
    }
    memcpy(registers, ikat.registers, IKAT_REGISTER_COUNT);
    for (channel = 0; channel < CHANNEL_COUNT; channel++)
        remaining[channel] = (uint8_t)response_remaining(channel);
    *pointer_time_ns = (double)ikat.pointer_elapsed_ns;
    *cursor_packets = ikat.pointer_repeat_count;
}

static void emit_pointer_packet(void) {
    uint32_t input = cdi_input_get();
    uint32_t directions = input & (CDI_INPUT_LEFT | CDI_INPUT_UP |
                                    CDI_INPUT_RIGHT | CDI_INPUT_DOWN);
    uint32_t buttons_changed = (input ^ ikat.previous_input) &
                               (CDI_INPUT_BTN1 | CDI_INPUT_BTN2);
    int speed;
    int x = 0;
    int y = 0;
    int mouse_x, mouse_y;

    if (directions) ikat.pointer_repeat_count++;
    else ikat.pointer_repeat_count = 0;
    speed = ikat.pointer_repeat_count + (ikat.pointer_repeat_count & 1);
    if (speed > 8) speed = 8;

    if (input & CDI_INPUT_LEFT) x = -speed;
    else if (input & CDI_INPUT_RIGHT) x = speed;
    if (input & CDI_INPUT_UP) y = -speed;
    else if (input & CDI_INPUT_DOWN) y = speed;

    /* The IKAT report carries signed eight-bit X/Y split across the header and
     * payload. Consume only what fits after keyboard/controller motion; any
     * remaining host-relative movement stays queued for the next 25 ms report. */
    cdi_input_take_relative(&mouse_x, &mouse_y,
                            -128 - x, 127 - x, -128 - y, 127 - y);
    x += mouse_x;
    y += mouse_y;

    if (directions || x || y || buttons_changed) {
        uint8_t packet[4];
        uint8_t encoded_x = (uint8_t)(int8_t)x;
        uint8_t encoded_y = (uint8_t)(int8_t)y;
        packet[0] = (uint8_t)(0x40u |
                    ((input & CDI_INPUT_BTN1) ? 0x20u : 0u) |
                    ((input & CDI_INPUT_BTN2) ? 0x10u : 0u) |
                    ((encoded_y >> 4) & 0x0Cu) |
                    ((encoded_x >> 6) & 0x03u));
        packet[1] = (uint8_t)(encoded_x & 0x3Fu);
        packet[2] = (uint8_t)(encoded_y & 0x3Fu);
        packet[3] = 0;
        publish_and_interrupt(CHANNEL_A, packet, sizeof packet);
    }
    ikat.previous_input = input;
    cdi_input_acknowledge_mouse_buttons();
}

void slave_increment_time(double nanoseconds) {
    int channel;
    uint64_t generation;
    ensure_initialized();

    generation = cdi_media_generation();
    if (generation != ikat.media_generation) {
        uint8_t present = (uint8_t)cdi_media_present();
        uint8_t status[4];
        ikat.media_generation = generation;
        record_event(CDI_IKAT_MEDIA, CHANNEL_D, &present, 1);
        make_disc_status(status);
        publish_and_interrupt(CHANNEL_D, status, sizeof status);
    }

    ikat.pointer_elapsed_ns += (uint64_t)nanoseconds;
    while (ikat.pointer_elapsed_ns >= 25000000u) {
        ikat.pointer_elapsed_ns -= 25000000u;
        emit_pointer_packet();
    }

    for (channel = 0; channel < CHANNEL_COUNT; channel++) {
        IkatChannel *queue = &ikat.channel[channel];
        if (queue->deferred_pending && g_frame_count >= queue->deferred_frame) {
            publish_and_interrupt(channel, queue->deferred, queue->deferred_length);
            queue->deferred_pending = 0;
            queue->deferred_frame = 0;
        }
    }
}

static uint8_t read_register(unsigned reg) {
    if (reg >= REG_OUTPUT_A && reg <= REG_OUTPUT_D) {
        int channel = (int)(reg - REG_OUTPUT_A);
        IkatChannel *queue = &ikat.channel[channel];
        if (response_remaining(channel)) {
            uint8_t value = queue->response[queue->response_position++];
            ikat.registers[reg] = value;
            record_event(CDI_IKAT_READ, channel, &value, 1);
            if (!response_remaining(channel))
                ikat.registers[REG_INTERRUPT_STATUS] &=
                    (uint8_t)~interrupt_bit[channel];
            update_channel_status(channel);
        }
    }
    return ikat.registers[reg];
}

static void write_register(unsigned reg, uint8_t value) {
    if (reg == REG_INTERRUPT_MASK) {
        ikat.registers[reg] = value;
        if (ikat.registers[REG_INTERRUPT_STATUS] & value) cdi_irq_raise(2);
        return;
    }
    if (reg <= REG_INPUT_D) {
        int channel = (int)reg;
        IkatChannel *queue = &ikat.channel[channel];
        ikat.registers[reg] = value;
        if (queue->command_length < CHANNEL_BUFFER_BYTES)
            queue->command[queue->command_length++] = value;
        if (channel == CHANNEL_C) handle_channel_c();
        else if (channel == CHANNEL_D) handle_channel_d();
    }
}

static int register_from_address(uint32_t address) {
    return (int)((address - CDI_SLAVE_BASE) >> 1);
}

uint32_t slave_read(uint32_t address, int size) {
    int reg;
    ensure_initialized();
    reg = register_from_address(address);
    if (reg < 0 || reg >= IKAT_REGISTER_COUNT) {
        fprintf(stderr, "[ikat] read%d @ $%08X: register %d out of range\n",
                size * 8, address, reg);
        return 0;
    }
    return read_register((unsigned)reg);
}

void slave_write(uint32_t address, uint32_t value, int size) {
    int reg;
    ensure_initialized();
    reg = register_from_address(address);
    if (reg < 0 || reg >= IKAT_REGISTER_COUNT) {
        fprintf(stderr,
                "[ikat] write%d @ $%08X = $%X: register %d out of range\n",
                size * 8, address, value, reg);
        return;
    }
    write_register((unsigned)reg, (uint8_t)value);
}
