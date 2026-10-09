#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t sha256[32];
    uint32_t offset;
    bool matched;
} CdiModuleSeed;
typedef struct {
    CdiModuleSeed *items;
    size_t count;
} CdiModuleSeeds;

/* A line contains the full module SHA-256 and an even hexadecimal offset.
 * RAM addresses and module names deliberately cannot identify an entry. */
bool cdi_module_seeds_read(const char *path, CdiModuleSeeds *out);
void cdi_module_seeds_free(CdiModuleSeeds *seeds);
bool cdi_module_seeds_merge(CdiModuleSeeds *seeds, const uint8_t digest[32],
                            uint32_t metadata_end, uint32_t code_end,
                            uint32_t *entries, size_t capacity, int *count);
