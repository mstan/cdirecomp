#include "cdi_runtime.h"

#include <stdint.h>
#include <stdio.h>

static int failures;
static int irq_count;
static uint8_t last_irq_level;

void cdi_irq_raise_onchip_level(uint8_t level) {
    irq_count++;
    last_irq_level = level;
}

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

int main(void) {
    periph_reset();
    CHECK(periph_read(0x80002013u, 1) == 0x04u);

    periph_write(0x80001001u, 0xFFu, 1);
    CHECK(periph_lir() == 0x77u);

    periph_write(0x80002019u, 0x41u, 1);
    CHECK(periph_read(0x80002013u, 1) == 0x0Cu);
    CHECK(periph_read(0x80002019u, 1) == 0x41u);

    periph_write(0x80008000u, 0xAAu, 1);
    CHECK(periph_read(0x80008000u, 1) == 0);

    periph_write(0x80002022u, 0x1234u, 2);
    periph_write(0x80002024u, 0xFFFFu, 2);
    periph_write(0x80002045u, 5, 1);
    periph_increment_timer(95);
    CHECK(periph_read(0x80002024u, 2) == 0xFFFFu);
    periph_increment_timer(1);
    CHECK(periph_read(0x80002024u, 2) == 0x1234u);
    CHECK((periph_read(0x80002020u, 1) & 0x80u) != 0);
    CHECK(irq_count == 1 && last_irq_level == 5);

    periph_write(0x80002020u, 0x80u, 1);
    CHECK((periph_read(0x80002020u, 1) & 0x80u) == 0);
    periph_write(0x80002020u, 0x80u, 1);
    CHECK((periph_read(0x80002020u, 1) & 0x80u) == 0);

    if (failures) {
        fprintf(stderr, "SCC68070 peripheral tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("SCC68070 peripheral tests: PASS");
    return 0;
}
