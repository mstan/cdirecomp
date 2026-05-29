/*
 * disc_parser.c — CD-i disc image + OS-9/68000 module inventory.
 * See disc_parser.h for the model and current limitations.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "disc_parser.h"
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* ---- .cue parsing (minimal) ---------------------------------------------
 * Pull the referenced binary file name and the first track's MODE from a
 * trivial single-FILE cue. Anything we can't parse falls back to treating the
 * passed path as a raw .bin. */
static bool cue_resolve_bin(const char *cue_path, char *bin_out, size_t bin_cap,
                            int *mode_out) {
    FILE *f = fopen(cue_path, "rb");
    if (!f) return false;
    char line[1024];
    bool got_file = false;
    *mode_out = 2; /* CD-i default */
    /* Directory of the cue, so a relative FILE resolves correctly. */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", cue_path);
    char *slash = dir, *p = dir;
    for (; *p; p++) if (*p == '/' || *p == '\\') slash = p + 1;
    *slash = '\0';
    while (fgets(line, sizeof(line), f)) {
        char *fq = strstr(line, "FILE");
        if (fq) {
            char *q1 = strchr(line, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) {
                    *q2 = '\0';
                    snprintf(bin_out, bin_cap, "%s%s", dir, q1 + 1);
                    got_file = true;
                }
            }
        }
        char *mq = strstr(line, "MODE");
        if (mq && mq[4]) *mode_out = (mq[4] == '1') ? 1 : 2;
    }
    fclose(f);
    return got_file;
}

bool cdi_disc_open(const char *path, CdiDisc *out) {
    memset(out, 0, sizeof(*out));
    out->track_mode = 2;

    const char *dot = strrchr(path, '.');
    bool is_cue = dot && (strcmp(dot, ".cue") == 0 || strcmp(dot, ".CUE") == 0);

    if (is_cue) {
        if (!cue_resolve_bin(path, out->bin_path, sizeof(out->bin_path), &out->track_mode)) {
            fprintf(stderr, "[disc] cue had no FILE entry: %s\n", path);
            return false;
        }
    } else {
        snprintf(out->bin_path, sizeof(out->bin_path), "%s", path);
    }

    out->bin = fopen(out->bin_path, "rb");
    if (!out->bin) {
        fprintf(stderr, "[disc] cannot open bin: %s\n", out->bin_path);
        return false;
    }
    if (fseek(out->bin, 0, SEEK_END) != 0) { fclose(out->bin); out->bin = NULL; return false; }
    long sz = ftell(out->bin);              /* CD images are < 2GB; long is fine */
    if (sz < 0) { fclose(out->bin); out->bin = NULL; return false; }
    out->bin_size = (uint64_t)sz;
    out->sector_count = (uint32_t)(out->bin_size / CDI_RAW_SECTOR_SIZE);
    rewind(out->bin);
    return true;
}

void cdi_disc_close(CdiDisc *d) {
    if (d && d->bin) { fclose(d->bin); d->bin = NULL; }
}

bool cdi_read_sector_form1(CdiDisc *d, uint32_t lba, uint8_t buf[CDI_MODE2_FORM1_DATA]) {
    if (lba >= d->sector_count) return false;
    long off = (long)lba * CDI_RAW_SECTOR_SIZE + CDI_SECTOR_DATA_OFF;
    if (fseek(d->bin, off, SEEK_SET) != 0) return false;
    return fread(buf, 1, CDI_MODE2_FORM1_DATA, d->bin) == CDI_MODE2_FORM1_DATA;
}

bool cdi_read_volume_descriptor(CdiDisc *d) {
    uint8_t s[CDI_MODE2_FORM1_DATA];
    if (!cdi_read_sector_form1(d, CDI_VOLUME_DESC_LBA, s)) {
        fprintf(stderr, "[disc] could not read volume descriptor (LBA %u)\n", CDI_VOLUME_DESC_LBA);
        return false;
    }
    char id[6] = {0};
    memcpy(id, &s[1], 5);
    printf("[disc] Volume descriptor @LBA16: type=0x%02X std_id=\"%s\"\n", s[0], id);
    return true;
}

uint32_t os9_module_crc24(const uint8_t *data, uint32_t len) {
    /* Standard OS-9 CRC: poly 0x800063, seeded 0xFFFFFF, MSB-first. */
    uint32_t crc = 0x00FFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 16;
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if (crc & 0x01000000u) crc ^= 0x800063u;
        }
        crc &= 0x00FFFFFFu;
    }
    return crc;
}

/* OS-9/68000 module header check word (M$Parity, offset 0x2E): the XOR of all
 * header words [0x00..0x2E] must equal 0xFFFF on a valid header. */
static bool os9_header_parity_ok(const uint8_t *m, uint32_t avail) {
    if (avail < OS9_MODULE_HDR_SIZE) return false;
    uint16_t x = 0;
    for (uint32_t o = 0; o <= 0x2E; o += 2) x ^= be16(m + o);
    return x == 0xFFFFu;
}

int os9_scan_buffer(const uint8_t *buf, uint64_t len, Os9Module *out, int max_out) {
    int found = 0;
    for (uint64_t o = 0; o + OS9_MODULE_HDR_SIZE <= len; o += 2) {
        if (!(buf[o] == 0x4A && buf[o + 1] == 0xFC)) continue;
        uint32_t avail = (uint32_t)((len - o > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (len - o));
        if (!os9_header_parity_ok(buf + o, avail)) continue;  /* reject 0x4AFC false hits */

        if (found < max_out) {
            Os9Module *mod = &out[found];
            memset(mod, 0, sizeof(*mod));
            mod->logical_offset    = o;
            mod->lba               = 0;     /* caller maps offset -> LBA / ROM addr */
            mod->size              = be32(buf + o + 0x04);
            mod->type              = buf[o + 0x12];
            mod->lang              = buf[o + 0x13];
            mod->header_parity_ok  = true;
            uint32_t nameoff       = be32(buf + o + 0x0C);
            if (nameoff && (o + nameoff) < len) {
                const uint8_t *ns = buf + o + nameoff;
                uint64_t room = len - (o + nameoff);
                uint64_t k = 0;
                for (; k < 63 && k < room; k++) {
                    uint8_t c = ns[k];
                    mod->name[k] = (char)(c & 0x7F);     /* strip high-bit terminator flag */
                    if (c & 0x80) { k++; break; }        /* high bit set = last char */
                }
                mod->name[k < 63 ? k : 63] = '\0';
            }
            if (mod->size >= OS9_MODULE_HDR_SIZE && (o + mod->size) <= len)
                mod->crc_ok = (os9_module_crc24(buf + o, mod->size) == 0);
        }
        found++;
    }
    return found;
}

int cdi_scan_os9_modules(CdiDisc *d, Os9Module *out, int max_out) {
    /* Build the concatenated Form-1 user-data stream so module headers that
     * straddle a sector boundary are scanned correctly. ~320MB for a full
     * disc — acceptable for an offline recompiler tool. Walking the CD-i
     * directory to bound the scan to actual files is a TODO (MC-CDI-003). */
    uint64_t stream_len = (uint64_t)d->sector_count * CDI_MODE2_FORM1_DATA;
    uint8_t *buf = (uint8_t *)malloc(stream_len);
    if (!buf) {
        fprintf(stderr, "[disc] OOM allocating %.1f MB stream for module scan\n",
                (double)stream_len / (1024.0 * 1024.0));
        return 0;
    }
    for (uint32_t lba = 0; lba < d->sector_count; lba++) {
        if (!cdi_read_sector_form1(d, lba, buf + (uint64_t)lba * CDI_MODE2_FORM1_DATA)) {
            /* Short read (audio track / form-2 region): zero-fill and continue. */
            memset(buf + (uint64_t)lba * CDI_MODE2_FORM1_DATA, 0, CDI_MODE2_FORM1_DATA);
        }
    }

    int n = os9_scan_buffer(buf, stream_len, out, max_out);
    int fill = n < max_out ? n : max_out;
    for (int i = 0; i < fill; i++)
        out[i].lba = (uint32_t)(out[i].logical_offset / CDI_MODE2_FORM1_DATA);

    free(buf);
    return n;
}
