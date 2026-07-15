#include "m68k_arith.h"

enum {
    CCR_C = 0x01,
    CCR_V = 0x02,
    CCR_Z = 0x04,
    CCR_N = 0x08
};

M68kDivideStatus m68k_divide_word(uint32_t dividend, uint16_t divisor,
                                  int signed_operation,
                                  uint32_t *packed_result, uint16_t *ccr) {
    uint16_t quotient_bits;
    uint16_t remainder_bits;

    *ccr = 0;
    if (divisor == 0)
        return M68K_DIVIDE_BY_ZERO;

    if (signed_operation) {
        int64_t quotient = (int64_t)(int32_t)dividend /
                           (int64_t)(int16_t)divisor;
        if (quotient < -32768 || quotient > 32767) {
            *ccr = CCR_N | CCR_V;
            return M68K_DIVIDE_OVERFLOW;
        }
        quotient_bits = (uint16_t)(int16_t)quotient;
        remainder_bits = (uint16_t)(int16_t)(
            (int64_t)(int32_t)dividend % (int64_t)(int16_t)divisor);
    } else {
        uint32_t quotient = dividend / divisor;
        if (quotient > 0xFFFFu) {
            *ccr = CCR_N | CCR_V;
            return M68K_DIVIDE_OVERFLOW;
        }
        quotient_bits = (uint16_t)quotient;
        remainder_bits = (uint16_t)(dividend % divisor);
    }

    *packed_result = ((uint32_t)remainder_bits << 16) | quotient_bits;
    if (quotient_bits == 0)
        *ccr |= CCR_Z;
    if (quotient_bits & 0x8000u)
        *ccr |= CCR_N;
    return M68K_DIVIDE_OK;
}
