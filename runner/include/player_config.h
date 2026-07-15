#pragma once

#include <stddef.h>

typedef struct CdiPlayerConfig {
    int capture_mouse;
    int sync_host_on_startup;
} CdiPlayerConfig;

void cdi_player_config_defaults(CdiPlayerConfig *config);

/* Load/save an explicit path. Load returns 1 on success, 0 when the file does
 * not exist, and -1 for malformed or unreadable input. */
int cdi_player_config_load(const char *path, CdiPlayerConfig *config);
int cdi_player_config_save(const char *path, const CdiPlayerConfig *config);

/* SDL supplies the per-user, cross-platform preference directory and creates
 * it when necessary. */
int cdi_player_config_default_path(char *path, size_t capacity);
