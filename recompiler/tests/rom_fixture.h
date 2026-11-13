#pragma once
#include "rom_parser.h"
bool rom_parse(const char *path, GenesisRom *out);
void rom_free(GenesisRom *rom);
