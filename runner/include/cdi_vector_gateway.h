#pragma once
#include <stdint.h>
/* OS-9 creates PEA (vector).W / JMP (native ROM).L exception thunks and JMP.L
 * library-vector tables in RAM. Own those complete gateways without
 * interpreting transient guest code. */
int cdi_vector_gateway_has_address(uint32_t address);
int cdi_vector_gateway_dispatch(uint32_t address);
