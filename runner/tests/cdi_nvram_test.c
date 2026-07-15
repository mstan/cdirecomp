#include "cdi_runtime.h"

#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static const uint8_t key[8] = {
    0xC5, 0x3A, 0xA3, 0x5C, 0xC5, 0x3A, 0xA3, 0x5C
};

static void unlock(void) {
    unsigned i, bit;
    for (i = 0; i < 8; i++)
        for (bit = 0; bit < 8; bit++)
            nvram_set_byte(0, (uint8_t)((key[i] >> bit) & 1u));
}

static void read_clock(uint8_t out[8]) {
    unsigned reg, bit;
    unlock();
    for (reg = 0; reg < 8; reg++) {
        out[reg] = 0;
        for (bit = 0; bit < 8; bit++)
            out[reg] |= (uint8_t)(nvram_get_byte(0) << bit);
    }
}

static void write_clock(const uint8_t in[8]) {
    unsigned reg, bit;
    unlock();
    for (reg = 0; reg < 8; reg++)
        for (bit = 0; bit < 8; bit++)
            nvram_set_byte(0, (uint8_t)((in[reg] >> bit) & 1u));
}

int main(void) {
    uint8_t clock[8];

    nvram_reset();
    CHECK(nvram_get_byte(0x1234) == 0xFF);
    nvram_set_byte(0x1234, 0x5A);
    CHECK(nvram_get_byte(0x1234) == 0x5A);

    read_clock(clock);
    CHECK(clock[0] == 0x00);
    CHECK(clock[1] == 0x00);
    CHECK(clock[2] == 0x00);
    CHECK(clock[3] == 0x00);
    CHECK((clock[4] & 7u) == 7u);
    CHECK(clock[5] == 0x01);
    CHECK(clock[6] == 0x01);
    CHECK(clock[7] == 0x89);

    nvram_increment_clock(1230000000.0);
    read_clock(clock);
    CHECK(clock[0] == 0x23);
    CHECK(clock[1] == 0x01);

    clock[0] = 0x99;
    clock[1] = 0x59;
    clock[2] = 0x59;
    clock[3] = 0x23;
    clock[4] = 0x01;
    clock[5] = 0x28;
    clock[6] = 0x02;
    clock[7] = 0x00;
    write_clock(clock);
    nvram_increment_clock(10000000.0);
    read_clock(clock);
    CHECK(clock[0] == 0x00);
    CHECK(clock[1] == 0x00);
    CHECK(clock[2] == 0x00);
    CHECK(clock[3] == 0x00);
    CHECK(clock[5] == 0x29); /* 2000 is a leap year. */

    clock[4] |= 0x20;
    write_clock(clock);
    nvram_increment_clock(5000000000.0);
    read_clock(clock);
    CHECK(clock[1] == 0x00);

    if (failures) {
        fprintf(stderr, "DS1216 tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("DS1216 tests: PASS");
    return 0;
}
