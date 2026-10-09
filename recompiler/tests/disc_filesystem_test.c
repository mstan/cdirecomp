#include "disc_parser.h"
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); failures++; \
} } while (0)

static const uint8_t module[52] = {
    0x4a, 0xfc, 0, 0, 0, 0, 0, 0x34,
    0, 0, 0, 0, 0, 0, 0, 0x30,
    0, 0, 4, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0xb1, 7,
    0xf8, 0xfe, 0x0d, 0xad
};

static void put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}

static void sector(CdiDisc *disc, unsigned lba, uint8_t submode, const uint8_t *payload) {
    uint8_t raw[CDI_RAW_SECTOR_SIZE] = {0};
    raw[15] = 2;
    raw[18] = raw[22] = submode;
    memcpy(raw + 24, payload, 2048);
    CHECK(fseek(disc->bin, (long)lba * sizeof raw, SEEK_SET) == 0);
    CHECK(fwrite(raw, 1, sizeof raw, disc->bin) == sizeof raw);
}

static unsigned record(uint8_t *p, const char *name, unsigned name_len,
                       unsigned lba, unsigned size, int directory, int cdi) {
    unsigned end = 33u + name_len + ((name_len & 1u) ? 0u : 1u);
    unsigned length = end + (cdi ? 8u : 0u);
    memset(p, 0, length);
    p[0] = (uint8_t)length;
    put32(p + 6, lba); put32(p + 14, size);
    p[25] = directory ? 2 : 0;
    p[32] = (uint8_t)name_len;
    memcpy(p + 33, name, name_len);
    if (cdi) {
        p[25] = 0;
        p[end + 2] = directory ? 0x85 : 0x05;
        p[end + 3] = 0x55;
    }
    return length;
}

static CdiDisc fixture(int cdi) {
    CdiDisc disc = {0};
    disc.bin = tmpfile();
    CHECK(disc.bin != NULL);
    disc.sector_count = 40;
    disc.bin_size = (uint64_t)disc.sector_count * CDI_RAW_SECTOR_SIZE;
    uint8_t payload[2048] = {0};
    for (unsigned i = 0; i < 40; i++) sector(&disc, i, 8, payload);
    uint8_t descriptor[2048] = {0}, directory[2048] = {0};
    descriptor[0] = descriptor[6] = 1;
    memcpy(descriptor + 1, cdi ? "CD-I " : "CD001", 5);
    if (cdi) {
        put32(descriptor + 136, 10);
        put32(descriptor + 148, 18);
        uint8_t table[2048] = {0};
        table[0] = 1; table[7] = 1; put32(table + 2, 20);
        sector(&disc, 18, 8, table);
    } else record(descriptor + 156, "\0", 1, 20, 2048, 1, 0);
    unsigned offset = record(directory, "\0", 1, 20, 2048, 1, cdi);
    offset += record(directory + offset, "\1", 1, 20, 2048, 1, cdi);
    offset += record(directory + offset, "code", 4, 22, 2052, 0, cdi);
    offset += record(directory + offset, "short", 5, 24, 51, 0, cdi);
    offset += record(directory + offset, "audio", 5, 25, 2048, 0, cdi);
    (void)offset;
    sector(&disc, 16, 8, descriptor);
    sector(&disc, 20, 8, directory);
    memset(payload, 0, sizeof payload);
    memcpy(payload + 2000, module, 48);
    sector(&disc, 22, 8, payload);
    memset(payload, 0, sizeof payload);
    memcpy(payload, module + 48, 4);
    sector(&disc, 23, 8, payload);
    memset(payload, 0, sizeof payload);
    memcpy(payload, module, sizeof module);
    sector(&disc, 24, 8, payload); /* declared file truncates footer */
    sector(&disc, 25, 0x64, payload); /* valid header in Form-2 must be ignored */
    sector(&disc, 26, 8, payload); /* valid header outside any file */
    return disc;
}

int main(void) {
    for (int cdi = 0; cdi <= 1; cdi++) {
        CdiDisc disc = fixture(cdi);
        if (!disc.bin) return 1;
        CdiFile files[3];
        Os9Module modules[2];
        CHECK(cdi_list_files(&disc, files, 3) == 3);
        CHECK(!strcmp(files[0].path, "code") && files[0].lba == 22);
        CHECK(cdi_scan_os9_modules(&disc, modules, 2) == 1);
        CHECK(modules[0].crc_ok && modules[0].lba == 22);
        CHECK(!strcmp(modules[0].file_path, "code") && modules[0].file_offset == 2000);
        uint8_t bytes[2048];
        CHECK(!cdi_read_sector_form1(&disc, 25, bytes));
        CHECK(cdi_read_file_form1(&disc, &files[2], bytes, sizeof bytes));
        CHECK(bytes[0] == 0 && bytes[1] == 0);
        files[0].lba = 39;
        CHECK(!cdi_read_file_form1(&disc, &files[0], bytes, sizeof bytes));
        /* A Form-2 directory is invalid, never an empty successful listing. */
        memset(bytes, 0, sizeof bytes);
        sector(&disc, 20, 0x64, bytes);
        CHECK(cdi_list_files(&disc, files, 3) == -1);
        cdi_disc_close(&disc);
    }
    CdiDisc disc = fixture(1);
    uint8_t bytes[2048];
    CHECK(cdi_read_sector_form1(&disc, 20, bytes));
    bytes[0] = 255; /* claims bytes beyond the special record's identifier */
    bytes[32] = 250;
    sector(&disc, 20, 8, bytes);
    CHECK(cdi_list_files(&disc, NULL, 0) == -1);
    cdi_disc_close(&disc);
    return failures ? 1 : 0;
}
