#pragma once
#include "rom_parser.h"

/* CIAP's private A2 device-state callback vector, not a generic LEA rule.
 * Returns unique, guarded native entry candidates or -1 on capacity failure. */
int cdi_driver_callbacks(const GenesisRom *rom, const char *module_name,
                         unsigned module_type, unsigned module_lang,
                         uint32_t module_begin, uint32_t module_size,
                         uint32_t *entries, int capacity);
