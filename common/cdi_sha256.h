#pragma once
#include <stddef.h>
#include <stdint.h>

/* Full byte identity of a module, independent of its load address. */
void cdi_sha256(const uint8_t *data, size_t size, uint8_t digest[32]);
/* Stream a file without allocating its contents. Returns 1 on success. */
int cdi_sha256_file(const char *path, uint8_t digest[32]);
