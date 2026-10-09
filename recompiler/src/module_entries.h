#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Subr layouts are application-defined. named_offset_pairs selects the
 * observed null-terminated (name offset, code offset) table used by this
 * disc. It is a compiler input format, never an OS-9 runtime replacement. */
int cdi_module_entries(const uint8_t *data,uint32_t size,uint8_t type,
                       bool named_offset_pairs,uint32_t *entries,int capacity,
                       uint32_t *metadata_end);
