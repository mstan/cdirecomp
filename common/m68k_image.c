/* Consumer-owned byte access adapter for the shared 68000 frontend. */
#include "rom_parser.h"

uint8_t rom_read8(const GenesisRom *rom, uint32_t addr) {
    if (addr < rom->rom_size) return rom->rom_data[addr];
    return 0xFF;
}

uint16_t rom_read16(const GenesisRom *rom, uint32_t addr) {
    if (addr + 1 < rom->rom_size)
        return (uint16_t)(rom->rom_data[addr] << 8 | rom->rom_data[addr + 1]);
    return 0xFFFF;
}

uint32_t rom_read32(const GenesisRom *rom, uint32_t addr) {
    if (addr + 3 < rom->rom_size)
        return (uint32_t)(rom->rom_data[addr]   << 24 | rom->rom_data[addr+1] << 16 |
                          rom->rom_data[addr+2] << 8  | rom->rom_data[addr+3]);
    return 0xFFFFFFFF;
}
