#pragma once

#include <stdint.h>

typedef enum M68kDivideStatus {
    M68K_DIVIDE_OK = 0,
    M68K_DIVIDE_BY_ZERO,
    M68K_DIVIDE_OVERFLOW
} M68kDivideStatus;

/* Execute the arithmetic portion of 68000 DIVU.W or DIVS.W. On success,
 * packed_result is remainder:quotient and ccr contains N/Z/V/C bits only.
 * Overflow and divide-by-zero leave packed_result untouched. */
M68kDivideStatus m68k_divide_word(uint32_t dividend, uint16_t divisor,
                                  int signed_operation,
                                  uint32_t *packed_result, uint16_t *ccr);
