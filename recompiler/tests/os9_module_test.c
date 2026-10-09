#include "disc_parser.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); failures++; \
} } while (0)

int main(void) {
    /* Independently encoded 52-byte Data module named x, with complemented
     * CRC footer FE 0D AD. No copyrighted fixture or runtime generator. */
    uint8_t module[52] = {
        0x4a, 0xfc, 0, 0, 0, 0, 0, 0x34,
        0, 0, 0, 0, 0, 0, 0, 0x30,
        0, 0, 4, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0xb1, 7,
        0xf8, 0xfe, 0x0d, 0xad
    };
    Os9Module out[2];
    CHECK(os9_module_crc24((const uint8_t *)"123456789", 9) == 0xDFF05Au);
    CHECK(os9_module_crc24(module, sizeof module) == OS9_MODULE_CRC_RESIDUE);
    CHECK(os9_scan_buffer(module, sizeof module, out, 2) == 1);
    CHECK(out[0].crc_ok && out[0].header_parity_ok);
    CHECK(out[0].size == sizeof module && strcmp(out[0].name, "x") == 0);
    CHECK(out[0].type == 4 && out[0].lang == 0);
    CHECK(os9_scan_buffer(module, sizeof module - 1, out, 2) == 0);
    module[51] ^= 1;
    CHECK(os9_scan_buffer(module, sizeof module, out, 2) == 1);
    CHECK(!out[0].crc_ok);
    module[46] ^= 1;
    CHECK(os9_scan_buffer(module, sizeof module, out, 2) == 0);
    return failures ? 1 : 0;
}
