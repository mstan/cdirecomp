#define _CRT_SECURE_NO_WARNINGS
#include "cdi_media.h"
#include "disc_parser.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static CdiDisc s_disc;
static char s_source_path[1024];
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static atomic_int s_present;
static atomic_ullong s_generation;

static void lock_media(void) {
    while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) { }
}

static void unlock_media(void) {
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
}

void cdi_media_init(void) {
    memset(&s_disc, 0, sizeof s_disc);
    memset(s_source_path, 0, sizeof s_source_path);
    atomic_store_explicit(&s_present, 0, memory_order_relaxed);
    atomic_store_explicit(&s_generation, 0, memory_order_relaxed);
    atomic_flag_clear_explicit(&s_lock, memory_order_relaxed);
}

int cdi_media_mount(const char *path) {
    CdiDisc incoming;
    uint8_t descriptor[CDI_MODE2_FORM1_DATA];
    if (!path || !*path || !cdi_disc_open(path, &incoming)) return 0;
    if (incoming.track_mode != 2 || incoming.sector_count <= CDI_VOLUME_DESC_LBA ||
        !cdi_read_sector_form1(&incoming, CDI_VOLUME_DESC_LBA, descriptor) ||
        (memcmp(descriptor + 1, "CD-I ", 5) != 0 &&
         memcmp(descriptor + 1, "CD001", 5) != 0)) {
        fprintf(stderr, "[media] not a supported single-track Mode-2 CD-i image: %s\n", path);
        cdi_disc_close(&incoming);
        return 0;
    }

    lock_media();
    cdi_disc_close(&s_disc);
    s_disc = incoming;
    snprintf(s_source_path, sizeof s_source_path, "%s", path);
    atomic_store_explicit(&s_present, 1, memory_order_release);
    atomic_fetch_add_explicit(&s_generation, 1, memory_order_release);
    unlock_media();
    printf("[media] inserted %s (%u raw sectors)\n", path, incoming.sector_count);
    return 1;
}

void cdi_media_eject(void) {
    lock_media();
    if (!atomic_load_explicit(&s_present, memory_order_relaxed)) {
        unlock_media();
        return;
    }
    atomic_store_explicit(&s_present, 0, memory_order_release);
    cdi_disc_close(&s_disc);
    memset(&s_disc, 0, sizeof s_disc);
    memset(s_source_path, 0, sizeof s_source_path);
    atomic_fetch_add_explicit(&s_generation, 1, memory_order_release);
    unlock_media();
    printf("[media] ejected\n");
}

int cdi_media_present(void) {
    return atomic_load_explicit(&s_present, memory_order_acquire);
}

uint64_t cdi_media_generation(void) {
    return atomic_load_explicit(&s_generation, memory_order_acquire);
}

uint32_t cdi_media_sector_count(void) {
    uint32_t result;
    lock_media();
    result = s_disc.sector_count;
    unlock_media();
    return result;
}

int cdi_media_track_mode(void) {
    int result;
    lock_media();
    result = s_disc.track_mode;
    unlock_media();
    return result;
}

void cdi_media_path(char *dst, uint32_t capacity) {
    if (!dst || !capacity) return;
    lock_media();
    snprintf(dst, capacity, "%s", s_source_path);
    unlock_media();
}

int cdi_media_read_sector_form1(uint32_t lba, uint8_t dst[2048]) {
    int ok = 0;
    lock_media();
    if (atomic_load_explicit(&s_present, memory_order_relaxed) && s_disc.bin)
        ok = cdi_read_sector_form1(&s_disc, lba, dst);
    unlock_media();
    return ok;
}
