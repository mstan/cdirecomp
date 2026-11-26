#pragma once
#include <stdint.h>
/* The OS-9 kernel creates PEA (vector).W / JMP (native ROM).L thunks in RAM.
 * Own that complete gateway without interpreting transient guest code. */
int cdi_vector_gateway_has_address(uint32_t address);
int cdi_vector_gateway_dispatch(uint32_t address);
