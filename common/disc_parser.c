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
    uint8_t body[CDI_MODE2_SECTOR_BODY];
    if (!cdi_read_sector_body(d, lba, body)) return false;
    if (body[3] == 1u) {
        memcpy(buf, body + 4, CDI_MODE2_FORM1_DATA);
        return true;
    }
    if (body[3] != 2u || memcmp(body + 4, body + 8, 4) || (body[6] & 0x20u))
        return false;
    memcpy(buf, body + 12, CDI_MODE2_FORM1_DATA);
    return true;
}

static bool valid_extent(CdiDisc *d, uint32_t lba, uint32_t size) {
    uint64_t sectors = ((uint64_t)size + CDI_MODE2_FORM1_DATA - 1u) /
                       CDI_MODE2_FORM1_DATA;
    return d && d->bin && lba < d->sector_count &&
           sectors <= (uint64_t)d->sector_count - lba;
}

bool cdi_read_file_form1(CdiDisc *d, const CdiFile *file, uint8_t *buf, uint32_t capacity) {
    if (!file || !buf || capacity < file->size ||
        !valid_extent(d, file->lba, file->size)) return false;
    for (uint32_t offset = 0; offset < file->size;) {
        uint8_t body[CDI_MODE2_SECTOR_BODY];
        uint32_t count = file->size - offset;
        if (count > CDI_MODE2_FORM1_DATA) count = CDI_MODE2_FORM1_DATA;
        if (!cdi_read_sector_body(d, file->lba + offset / CDI_MODE2_FORM1_DATA, body))
            return false;
        if (body[3] == 1u) memcpy(buf + offset, body + 4, count);
        else if (body[3] == 2u && !memcmp(body + 4, body + 8, 4)) {
            if (body[6] & 0x20u) memset(buf + offset, 0, count);
            else memcpy(buf + offset, body + 12, count);
        } else return false;
        offset += count;
    }
    return true;
}

typedef struct {
    CdiDisc *disc;
    CdiFile *out;
    int capacity, count;
    bool cdi;
    uint32_t directories[256];
    unsigned directory_count;
} DirectoryWalk;

static bool walk_directory(DirectoryWalk *walk, uint32_t lba, uint32_t size,
                           const char *prefix, unsigned depth) {
    if (!size || size > 16u * 1024u * 1024u || depth > 16u ||
        !valid_extent(walk->disc, lba, size) || walk->directory_count >= 256u)
        return false;
    for (unsigned i = 0; i < walk->directory_count; i++)
        if (walk->directories[i] == lba) return false;
    walk->directories[walk->directory_count++] = lba;
    uint8_t *bytes = malloc(size);
    if (!bytes) return false;
    bool ok = true;
    for (uint32_t offset = 0; ok && offset < size;) {
        uint8_t sector[CDI_MODE2_FORM1_DATA];
        ok = cdi_read_sector_form1(walk->disc, lba + offset / CDI_MODE2_FORM1_DATA, sector);
        uint32_t count = size - offset;
        if (count > CDI_MODE2_FORM1_DATA) count = CDI_MODE2_FORM1_DATA;
        if (ok) memcpy(bytes + offset, sector, count);
        offset += count;
    }
    uint32_t offset = 0;
    while (ok && offset < size) {
        uint32_t record_size = bytes[offset];
        if (!record_size) {
            uint32_t next = offset + CDI_MODE2_FORM1_DATA -
                            offset % CDI_MODE2_FORM1_DATA;
            offset = next < size ? next : size;
            continue;
        }
        if (record_size < 34u || record_size > size - offset ||
            record_size > CDI_MODE2_FORM1_DATA - offset % CDI_MODE2_FORM1_DATA) {
            ok = false;
            break;
        }
        const uint8_t *record = bytes + offset;
        uint32_t name_size = record[32];
        uint32_t extension = 33u + name_size + ((name_size & 1u) ? 0u : 1u);
        if (!name_size || extension > record_size ||
            (walk->cdi && record_size - extension < 8u) || record[1]) {
            ok = false; /* extended-attribute blocks need an explicit reader */
            break;
        }
        if (name_size == 1u && record[33] <= 1u) {
            offset += record_size;
            continue;
        }
        CdiFile file = {0};
        file.lba = be32(record + 6);
        file.size = be32(record + 14);
        bool is_directory = walk->cdi
            ? (be16(record + extension + 2) & 0x8000u) != 0
            : (record[25] & 2u) != 0;
        if (!valid_extent(walk->disc, file.lba, file.size) ||
            (!walk->cdi && (record[25] & 0x80u)) || record[26] || record[27]) {
            ok = false; /* multi-extent/interleaved ISO allocation unsupported */
            break;
        }
        size_t prefix_size = strlen(prefix);
        if (prefix_size + name_size >= sizeof file.path) {
            ok = false;
            break;
        }
        memcpy(file.path, prefix, prefix_size);
        for (uint32_t i = 0; i < name_size; i++) {
            uint8_t ch = record[33u + i];
            if (!ch || ch == '/' || ch == '\\') { ok = false; break; }
            file.path[prefix_size + i] = (char)ch;
        }
        if (!ok) break;
        if (is_directory) {
            size_t length = strlen(file.path);
            if (length + 1 >= sizeof file.path) { ok = false; break; }
            file.path[length] = '/';
            ok = walk_directory(walk, file.lba, file.size, file.path, depth + 1u);
        } else {
            if (walk->count < walk->capacity) walk->out[walk->count] = file;
            walk->count++;
        }
        offset += record_size;
    }
    free(bytes);
    return ok;
}

int cdi_list_files(CdiDisc *d, CdiFile *out, int max_out) {
    uint8_t descriptor[CDI_MODE2_FORM1_DATA], root[CDI_MODE2_FORM1_DATA];
    if (max_out < 0 || (max_out && !out) ||
        !cdi_read_sector_form1(d, CDI_VOLUME_DESC_LBA, descriptor) ||
        descriptor[0] != 1u || descriptor[6] != 1u) return -1;
    bool cdi = !memcmp(descriptor + 1, "CD-I ", 5);
    if (!cdi && memcmp(descriptor + 1, "CD001", 5)) return -1;
    uint32_t root_lba, root_size;
    if (cdi) {
        /* CD-i leaves the ISO root record empty. The big-endian path table
         * supplies its root extent; the dot record supplies directory size. */
        uint32_t table_size = be32(descriptor + 136);
        uint32_t table_lba = be32(descriptor + 148);
        if (table_size < 10u || !valid_extent(d, table_lba, table_size) ||
            !cdi_read_sector_form1(d, table_lba, root) || root[0] != 1u ||
            root[1] || be16(root + 6) != 1u || root[8]) return -1;
        root_lba = be32(root + 2);
        if (!cdi_read_sector_form1(d, root_lba, root) || root[0] < 34u ||
            root[32] != 1u || root[33] || be32(root + 6) != root_lba) return -1;
        root_size = be32(root + 14);
    } else {
        if (descriptor[156] < 34u || descriptor[157]) return -1;
        root_lba = be32(descriptor + 162);
        root_size = be32(descriptor + 170);
    }
    DirectoryWalk walk = {0};
    walk.disc = d;
    walk.out = out;
    walk.capacity = max_out;
    walk.cdi = cdi;
    return walk_directory(&walk, root_lba, root_size, "", 0) ? walk.count : -1;
}

bool cdi_read_sector_body(CdiDisc *d, uint32_t lba,
                          uint8_t buf[CDI_MODE2_SECTOR_BODY]) {
    if (!d || !d->bin || lba >= d->sector_count) return false;
    long off = (long)lba * CDI_RAW_SECTOR_SIZE + CDI_SECTOR_BODY_OFF;
    if (fseek(d->bin, off, SEEK_SET) != 0) return false;
    return fread(buf, 1, CDI_MODE2_SECTOR_BODY, d->bin) ==
           CDI_MODE2_SECTOR_BODY;
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
        uint32_t module_size = be32(buf + o + 0x04);
        if (module_size < OS9_MODULE_HDR_SIZE + 3u || module_size > avail)
            continue;

        if (found < max_out) {
            Os9Module *mod = &out[found];
            memset(mod, 0, sizeof(*mod));
            mod->logical_offset    = o;
            mod->lba               = 0;     /* caller maps offset -> LBA / ROM addr */
            mod->size              = module_size;
            mod->type              = buf[o + 0x12];
            mod->lang              = buf[o + 0x13];
            mod->header_parity_ok  = true;
            uint32_t nameoff       = be32(buf + o + 0x0C);
            if (nameoff >= OS9_MODULE_HDR_SIZE && nameoff < module_size - 3u) {
                const uint8_t *ns = buf + o + nameoff;
                uint64_t room = module_size - 3u - nameoff;
                uint64_t k = 0;
                for (; k < 63 && k < room; k++) {
                    uint8_t c = ns[k];
                    if (!c) break;
                    mod->name[k] = (char)(c & 0x7F);     /* strip high-bit terminator flag */
                    if (c & 0x80) { k++; break; }        /* high bit set = last char */
                }
                mod->name[k < 63 ? k : 63] = '\0';
            }
            if (mod->size >= OS9_MODULE_HDR_SIZE && (o + mod->size) <= len)
                mod->crc_ok = (os9_module_crc24(buf + o, mod->size) ==
                               OS9_MODULE_CRC_RESIDUE);
        }
        found++;
    }
    return found;
}

int cdi_scan_os9_modules(CdiDisc *d, Os9Module *out, int max_out) {
    int files = cdi_list_files(d, NULL, 0);
    if (files < 0 || max_out < 0 || (max_out && !out)) return -1;
    CdiFile *entries = calloc(files ? (size_t)files : 1u, sizeof *entries);
    if (!entries || cdi_list_files(d, entries, files) != files) { free(entries); return -1; }
    int found = 0;
    for (int i = 0; i < files; i++) {
        CdiFile *file = &entries[i];
        if (!file->size) continue;
        uint8_t *bytes = malloc(file->size);
        if (!bytes || !cdi_read_file_form1(d, file, bytes, file->size)) {
            free(bytes); free(entries); return -1;
        }
        int remaining = found < max_out ? max_out - found : 0;
        int count = os9_scan_buffer(bytes, file->size, remaining ? out + found : NULL, remaining);
        int fill = count < remaining ? count : remaining;
        for (int j = 0; j < fill; j++) {
            Os9Module *module = &out[found + j];
            module->file_offset = (uint32_t)module->logical_offset;
            module->lba = file->lba + module->file_offset / CDI_MODE2_FORM1_DATA;
            module->logical_offset += (uint64_t)file->lba * CDI_MODE2_FORM1_DATA;
            snprintf(module->file_path, sizeof module->file_path, "%s", file->path);
        }
        free(bytes);
        found += count;
    }
    free(entries);
    return found;
}
