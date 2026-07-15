/*
 * Philips SCC68070 on-chip peripheral register block.
 *
 * Implemented from the SCC68070 User Manual, chapters 5, 7, 8 and 9.  The
 * currently exercised subset is the UART, 16-bit timer, interrupt priority
 * control, and passive storage for the remaining documented registers.
 */
#include "cdi_runtime.h"

#include <stdint.h>
#include <string.h>

enum {
    PERIPHERAL_BASE = 0x80001001u,
    PERIPHERAL_LAST = 0x80008080u,
    PERIPHERAL_BYTES = PERIPHERAL_LAST - PERIPHERAL_BASE + 1u
};

enum {
    REG_LIR   = 0x80001001u,
    REG_USR   = 0x80002013u,
    REG_UCR   = 0x80002017u,
    REG_UTHR  = 0x80002019u,
    REG_URHR  = 0x8000201Bu,
    REG_TSR   = 0x80002020u,
    REG_TCR   = 0x80002021u,
    REG_RRH   = 0x80002022u,
    REG_RRL   = 0x80002023u,
    REG_T0H   = 0x80002024u,
    REG_T0L   = 0x80002025u,
    REG_T1H   = 0x80002026u,
    REG_T1L   = 0x80002027u,
    REG_T2H   = 0x80002028u,
    REG_T2L   = 0x80002029u,
    REG_PICR1 = 0x80002045u,
    REG_MSR   = 0x80008000u
};

enum {
    UART_RX_READY = 0x01,
    UART_TX_READY = 0x04,
    UART_TX_EMPTY = 0x08
};

typedef struct {
    uint8_t bytes[PERIPHERAL_BYTES];
    uint32_t timer_prescale_cycles;
} SccPeripherals;

static SccPeripherals device;

static int mapped(uint32_t address) {
    return address >= PERIPHERAL_BASE && address <= PERIPHERAL_LAST;
}

static uint8_t *reg_ptr(uint32_t address) {
    return &device.bytes[address - PERIPHERAL_BASE];
}

static uint16_t load_counter(uint32_t high_address) {
    return (uint16_t)(((uint16_t)*reg_ptr(high_address) << 8) |
                      *reg_ptr(high_address + 1));
}

static void store_counter(uint32_t high_address, uint16_t value) {
    *reg_ptr(high_address) = (uint8_t)(value >> 8);
    *reg_ptr(high_address + 1) = (uint8_t)value;
}

void periph_reset(void) {
    memset(&device, 0, sizeof device);
    *reg_ptr(REG_USR) = UART_TX_READY;
}

uint8_t periph_lir(void) {
    return *reg_ptr(REG_LIR);
}

static void request_timer_interrupt(void) {
    uint8_t priority = (uint8_t)(*reg_ptr(REG_PICR1) & 7u);
    if (priority) cdi_irq_raise_onchip_level(priority);
}

static void clock_timer_once(void) {
    uint8_t control = *reg_ptr(REG_TCR);
    uint8_t *status = reg_ptr(REG_TSR);
    uint16_t timer0 = load_counter(REG_T0H);

    if (timer0 == 0xFFFFu) {
        timer0 = load_counter(REG_RRH);
        *status |= 0x80u;
        request_timer_interrupt();
    } else {
        timer0++;
    }
    store_counter(REG_T0H, timer0);

    if (control & 0x30u) {
        uint16_t timer1 = load_counter(REG_T1H);
        if (timer1 == 0xFFFFu) {
            *status |= 0x10u;
            request_timer_interrupt();
        } else if ((control & 0x30u) == 0x30u) {
            *status &= (uint8_t)~0x11u;
        }
        timer1++;
        if (timer1 == timer0) *status |= 0x40u;
        store_counter(REG_T1H, timer1);
    }

    if (control & 0x03u) {
        uint16_t timer2 = load_counter(REG_T2H);
        if (timer2 == 0xFFFFu) {
            *status |= 0x02u;
            request_timer_interrupt();
        } else if ((control & 0x03u) == 0x03u) {
            *status &= (uint8_t)~0x03u;
        }
        timer2++;
        if (timer2 == timer0) *status |= 0x08u;
        store_counter(REG_T2H, timer2);
    }
}

void periph_increment_timer(uint32_t cycles) {
    uint64_t accumulated = (uint64_t)device.timer_prescale_cycles + cycles;
    uint32_t ticks = (uint32_t)(accumulated / 96u);
    device.timer_prescale_cycles = (uint32_t)(accumulated % 96u);
    while (ticks--) clock_timer_once();
}

static uint8_t read_register(uint32_t address) {
    if (!mapped(address)) return 0;
    if (address == REG_URHR) {
        /* No external UART receiver is connected in the current machine. */
        *reg_ptr(REG_URHR) = 0;
        *reg_ptr(REG_USR) &= (uint8_t)~UART_RX_READY;
    }
    return *reg_ptr(address);
}

static void write_uart_command(uint8_t command) {
    switch (command & 0x70u) {
    case 0x20:
        *reg_ptr(REG_URHR) = 0;
        *reg_ptr(REG_USR) &= (uint8_t)~UART_RX_READY;
        break;
    case 0x30:
        *reg_ptr(REG_UTHR) = 0;
        *reg_ptr(REG_USR) |= UART_TX_READY | UART_TX_EMPTY;
        break;
    case 0x40:
        *reg_ptr(REG_USR) &= 0x0Fu;
        break;
    default:
        break;
    }
    *reg_ptr(REG_UCR) = command;
}

static void write_register(uint32_t address, uint8_t value) {
    if (!mapped(address)) return;

    switch (address) {
    case REG_LIR:
        *reg_ptr(address) = (uint8_t)(value & 0x77u);
        break;
    case REG_UCR:
        write_uart_command(value);
        break;
    case REG_UTHR:
        *reg_ptr(address) = value;
        *reg_ptr(REG_USR) |= UART_TX_EMPTY;
        break;
    case REG_TSR:
        *reg_ptr(address) &= (uint8_t)~value; /* write one to clear */
        break;
    case REG_MSR:
        break; /* read-only */
    default:
        *reg_ptr(address) = value;
        break;
    }
}

uint32_t periph_read(uint32_t address, int size) {
    uint32_t result = 0;
    int i;
    for (i = 0; i < size; i++)
        result = (result << 8) | read_register(address + (uint32_t)i);
    return result;
}

void periph_write(uint32_t address, uint32_t value, int size) {
    int i;
    for (i = 0; i < size; i++) {
        unsigned shift = 8u * (unsigned)(size - i - 1);
        write_register(address + (uint32_t)i, (uint8_t)(value >> shift));
    }
}
