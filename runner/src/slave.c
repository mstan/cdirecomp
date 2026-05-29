/*
 * slave.c — SLAVE i8051 microcontroller (skeleton).
 *
 * The SLAVE handles pointing-device / controller input and the front panel,
 * communicating with the SCC68070 over the on-chip I2C bus and a small
 * register window at $310000 (odd bytes). Hotel Mario reads the d-pad + two
 * buttons through it.
 *
 * Reference: external/CeDImu cores/ISlave + board Slave implementations.
 * TODO MC-CDI-014: input report protocol + interrupt.
 */
#include "cdi_runtime.h"
#include <stdio.h>
#include <stdlib.h>

uint32_t slave_read(uint32_t addr, int size) {
    fprintf(stderr, "[slave] read%d @ $%08X — input MCU not modelled "
                    "(TODO MC-CDI-014)\n", size * 8, addr);
    abort();
}
void slave_write(uint32_t addr, uint32_t val, int size) {
    fprintf(stderr, "[slave] write%d @ $%08X = $%X — input MCU not modelled "
                    "(TODO MC-CDI-014)\n", size * 8, addr, val);
    abort();
}
