/* Host CD media backing. Media changes are observed by IKAT on emulated time;
 * sector consumers use the same mounted image through this interface. */
#pragma once

#include <stdint.h>

void cdi_media_init(void);
int  cdi_media_mount(const char *cue_or_bin_path);
void cdi_media_eject(void);
int  cdi_media_present(void);
uint64_t cdi_media_generation(void);
uint32_t cdi_media_sector_count(void);
int  cdi_media_track_mode(void);
void cdi_media_path(char *dst, uint32_t capacity);
int  cdi_media_read_sector_form1(uint32_t lba, uint8_t dst[2048]);
