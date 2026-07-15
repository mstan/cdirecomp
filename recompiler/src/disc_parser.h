/*
 * disc_parser.h — CD-i (Green Book) disc + OS-9/68000 module parsing.
 *
 * CD-i program code does NOT live in a flat ROM the way a Genesis cartridge
 * does. A CD-i title is a Green Book Mode-2 CD whose data track contains a
 * CD-i/ISO-9660 file system; the executable payload is a set of OS-9/68000
 * relocatable *modules* (sync word 0x4AFC) that CD-RTOS loads and relocates
 * into RAM at run time. This parser inventories the disc image and locates
 * those modules — the first step before the shared 68000 frontend
 * (function_finder + code_generator) can be pointed at real code.
 *
 * Raw .bin images store 2352-byte sectors. The .cue describes the track
 * layout. We currently assume a single Mode-2 data track (the common CD-i
 * shape); multi-track / mixed-mode handling is a TODO.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define CDI_RAW_SECTOR_SIZE   2352u
#define CDI_MODE2_FORM1_DATA   2048u  /* Form-1 user data bytes */
#define CDI_MODE2_FORM2_DATA   2324u  /* Form-2 user data bytes */
#define CDI_MODE2_SECTOR_BODY  2340u  /* header(4) + subheader(8) + payload */
#define CDI_SECTOR_BODY_OFF      12u  /* bytes following the raw sync field */
#define CDI_SECTOR_DATA_OFF      24u  /* sync(12) + header(4) + subheader(8) */
#define CDI_VOLUME_DESC_LBA      16u  /* ISO-9660 / CD-i volume descriptor */
#define OS9_MODULE_SYNC      0x4AFCu  /* M$ID — also the 68K ILLEGAL opcode */
#define OS9_MODULE_HDR_SIZE   0x30u   /* standard OS-9/68000 module header */

typedef struct {
    char      bin_path[1024];
    FILE     *bin;
    uint64_t  bin_size;
    uint32_t  sector_count;   /* bin_size / 2352 */
    int       track_mode;     /* 1 or 2, parsed from the .cue (2 = CD-i) */
} CdiDisc;

/* One discovered OS-9/68000 module candidate. */
typedef struct {
    uint64_t logical_offset;  /* byte offset within the concatenated Form-1 stream */
    uint32_t lba;             /* sector containing the sync word */
    uint32_t size;            /* M$Size (module length in bytes) */
    uint8_t  type;            /* M$Type (e.g. Prog=0x1, Subr, Data, Device, ...) */
    uint8_t  lang;            /* M$Lang (e.g. 68000 objode = 0x1) */
    char     name[64];        /* M$Name string (high-bit terminator stripped) */
    bool     header_parity_ok;/* trusted validator: header check word == 0xFFFF */
    bool     crc_ok;          /* os9_module_crc24 residue == 0 — UNVERIFIED, see TODO.md */
} Os9Module;

/* Open from a .cue (preferred) or directly from a .bin. */
bool cdi_disc_open(const char *cue_or_bin_path, CdiDisc *out);
void cdi_disc_close(CdiDisc *d);

/* Copy the 2048 Form-1 user bytes of sector `lba` into `buf`. */
bool cdi_read_sector_form1(CdiDisc *d, uint32_t lba, uint8_t buf[CDI_MODE2_FORM1_DATA]);

/* Copy the 2340 bytes presented by the Mono-III/IV CIAP data buffers: the
 * Mode-2 header, duplicated subheader, payload, and trailing EDC/ECC bytes,
 * with only the 12-byte raw-sector sync field removed. */
bool cdi_read_sector_body(CdiDisc *d, uint32_t lba,
                          uint8_t buf[CDI_MODE2_SECTOR_BODY]);

/* Read + print the volume descriptor at LBA 16 (standard id "CD-I "/"CD001"). */
bool cdi_read_volume_descriptor(CdiDisc *d);

/* Core scan over a flat byte buffer, shared by the disc scan and the BIOS-ROM
 * scan: find 0x4AFC OS-9 modules validated by header parity. Fills
 * logical_offset = byte offset within `buf`. Returns count (fills up to max_out). */
int os9_scan_buffer(const uint8_t *buf, uint64_t len, Os9Module *out, int max_out);

/* Scan the concatenated Form-1 user stream for 0x4AFC module syncs, validating
 * each with the header parity word. Fills up to `max_out` entries; returns the
 * number of valid module headers found (header_parity_ok). */
int cdi_scan_os9_modules(CdiDisc *d, Os9Module *out, int max_out);

/* OS-9/68000 module CRC-24 (poly 0x800063, init 0xFFFFFF). The algorithm is
 * standard; the residue-equals-zero validity check still needs verification
 * against real Hotel Mario modules before we trust it (see TODO.md MC-CDI-002). */
uint32_t os9_module_crc24(const uint8_t *data, uint32_t len);
