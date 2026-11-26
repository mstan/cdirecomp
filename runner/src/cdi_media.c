#define _CRT_SECURE_NO_WARNINGS
#include "cdi_media.h"
#include "disc_parser.h"
#include "cdi_sha256.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static CdiDisc s_disc;

typedef struct MediaPage {uint32_t lba;uint8_t bytes[CDI_RAW_SECTOR_SIZE];struct MediaPage *next;} MediaPage;
static MediaPage *pages;
static void clear_pages(void) {while(pages){MediaPage *next=pages->next;free(pages);pages=next;}}
static MediaPage *find_page(uint32_t lba) {for(MediaPage *p=pages;p;p=p->next)if(p->lba==lba)return p;return NULL;}
static int raw_read(uint64_t address,void *data,size_t size) {
    if(!s_disc.bin || address>s_disc.bin_size || size>s_disc.bin_size-address)return 0;
    uint8_t *out=data;
    while(size){uint32_t lba=(uint32_t)(address/CDI_RAW_SECTOR_SIZE),offset=(uint32_t)(address%CDI_RAW_SECTOR_SIZE);size_t n=CDI_RAW_SECTOR_SIZE-offset;if(n>size)n=size;
        MediaPage *page=find_page(lba);
        if(page)memcpy(out,page->bytes+offset,n);
        else {if(fseek(s_disc.bin,(long)address,SEEK_SET)!=0 || fread(out,1,n,s_disc.bin)!=n)return 0;}
        address+=n;out+=n;size-=n;
    }return 1;
}

static char s_source_path[1024];
static char original_sha[65];
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
    clear_pages();original_sha[0]=0;
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
    clear_pages();
    cdi_disc_close(&s_disc);
    s_disc = incoming;
    original_sha[0]=0;
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
    clear_pages();
    cdi_disc_close(&s_disc);
    memset(&s_disc, 0, sizeof s_disc);
    memset(s_source_path, 0, sizeof s_source_path);
    original_sha[0]=0;
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
    if (atomic_load_explicit(&s_present, memory_order_relaxed) && s_disc.bin) {
        uint8_t raw[CDI_RAW_SECTOR_SIZE];
        ok=raw_read((uint64_t)lba*CDI_RAW_SECTOR_SIZE,raw,sizeof raw) &&
            raw[15]==2 && !memcmp(raw+16,raw+20,4) && !(raw[18]&0x20);
        if(ok)memcpy(dst,raw+CDI_SECTOR_DATA_OFF,2048);
    }
    unlock_media();
    return ok;
}

int cdi_media_read_sector_body(uint32_t lba, uint8_t dst[2340]) {
    int ok = 0;
    lock_media();
    if (atomic_load_explicit(&s_present, memory_order_relaxed) && s_disc.bin)
        ok = raw_read((uint64_t)lba*CDI_RAW_SECTOR_SIZE+CDI_SECTOR_BODY_OFF,dst,2340);
    unlock_media();
    return ok;
}

int cdi_media_mod_read(uint64_t address,void *bytes,size_t size) {
    lock_media();int ok=raw_read(address,bytes,size);unlock_media();return ok;
}
int cdi_media_original_sha256(char out[65]) {
    lock_media();int ok=0;
    if(s_disc.bin) {
        if(!original_sha[0]){uint8_t hash[32];if(cdi_sha256_file(s_disc.bin_path,hash))for(unsigned i=0;i<32;i++)sprintf(original_sha+2*i,"%02x",hash[i]);}
        if(original_sha[0]){memcpy(out,original_sha,sizeof original_sha);ok=1;}
    }
    unlock_media();return ok;
}
int cdi_media_mod_write(uint64_t address,const void *bytes,size_t size) {
    lock_media();
    if(!s_disc.bin || address>s_disc.bin_size || size>s_disc.bin_size-address){unlock_media();return 0;}
    /* Allocate every destination before changing bytes; core rollback can
     * restore accepted pages without allocation after a later operation fails. */
    if(size)for(uint64_t lba=address/CDI_RAW_SECTOR_SIZE;lba<=(address+size-1)/CDI_RAW_SECTOR_SIZE;lba++)if(!find_page((uint32_t)lba)) {
        MediaPage *page=malloc(sizeof *page);
        if(!page){unlock_media();return 0;}
        page->lba=(uint32_t)lba;
        if(!raw_read(lba*CDI_RAW_SECTOR_SIZE,page->bytes,sizeof page->bytes)){free(page);unlock_media();return 0;}
        page->next=pages;pages=page;
    }
    const uint8_t *in=bytes;
    while(size){uint32_t lba=(uint32_t)(address/CDI_RAW_SECTOR_SIZE),offset=(uint32_t)(address%CDI_RAW_SECTOR_SIZE);size_t n=CDI_RAW_SECTOR_SIZE-offset;if(n>size)n=size;
        memcpy(find_page(lba)->bytes+offset,in,n);address+=n;in+=n;size-=n;
    }
    unlock_media();return 1;
}
