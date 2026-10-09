#pragma once
#include "disc_parser.h"
#include <stdbool.h>
bool cdi_emit_modules(CdiDisc *disc, const Os9Module *modules, int count,
                      const char *directory, const char *module_filter,
                      bool named_offset_pairs, const char *seed_path);
