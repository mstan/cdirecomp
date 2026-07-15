#include "m68k_arith.h"

#include <stdint.h>
#include <stdio.h>

static int failures;

static void expect_case(const char *name, uint32_t dividend, uint16_t divisor,
                        int signed_operation, M68kDivideStatus want_status,
                        uint32_t want_result, uint16_t want_ccr) {
    uint32_t result = 0xA5A5A5A5u;
    uint16_t ccr = 0xFFFFu;
    M68kDivideStatus status = m68k_divide_word(
        dividend, divisor, signed_operation, &result, &ccr);
    if (status != want_status || ccr != want_ccr ||
        (status == M68K_DIVIDE_OK && result != want_result) ||
        (status != M68K_DIVIDE_OK && result != 0xA5A5A5A5u)) {
        fprintf(stderr,
                "FAIL %s: status=%d result=%08X ccr=%02X\n",
                name, status, result, ccr);
        failures++;
    }
}

int main(void) {
    expect_case("DIVU negative quotient bit", 0x00010000u, 2, 0,
                M68K_DIVIDE_OK, 0x00008000u, 0x08);
    expect_case("DIVU zero quotient", 1, 2, 0,
                M68K_DIVIDE_OK, 0x00010000u, 0x04);
    expect_case("DIVU overflow", 0xFFFFFFFFu, 1, 0,
                M68K_DIVIDE_OVERFLOW, 0, 0x0A);
    expect_case("DIVS negative quotient/remainder", (uint32_t)-7, 3, 1,
                M68K_DIVIDE_OK, 0xFFFFFFFEu, 0x08);
    expect_case("DIVS negative remainder", (uint32_t)-7, (uint16_t)-3, 1,
                M68K_DIVIDE_OK, 0xFFFF0002u, 0x00);
    expect_case("DIVS positive overflow", 32768, 1, 1,
                M68K_DIVIDE_OVERFLOW, 0, 0x0A);
    expect_case("DIVS INT_MIN overflow", 0x80000000u, (uint16_t)-1, 1,
                M68K_DIVIDE_OVERFLOW, 0, 0x0A);
    expect_case("divide by zero", 123, 0, 0,
                M68K_DIVIDE_BY_ZERO, 0, 0x00);

    if (failures) return 1;
    puts("m68k arithmetic tests passed");
    return 0;
}
