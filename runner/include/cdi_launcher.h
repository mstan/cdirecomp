#pragma once
#include "player_config.h"
/* Supplied by a game that integrates recomp-ui. 0 play, 1 quit, 2 failure. */
int cdi_launcher_run(const char *initial_bios, const char *initial_disc,
                     char *bios, size_t bios_capacity, char *disc, size_t disc_capacity,
                     CdiPlayerConfig *config, const char *config_path);
