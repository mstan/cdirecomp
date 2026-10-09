#pragma once
#include <stddef.h>
#include <stdint.h>

/* Full byte identity of a module, independent of its load address. */
void cdi_sha256(const uint8_t *data, size_t size, uint8_t digest[32]);
