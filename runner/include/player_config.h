#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct CdiPlayerConfig {
    int capture_mouse;
    int sync_host_on_startup;
    int fit_to_screen;
    int window_width;
    int fullscreen;
    int linear_filter;
    int volume;
    int enable_audio;
    int skip_launcher;
    int input_source; /* 1 keyboard, 2 gamepad */
    int key_bind[6];
    int pad_bind[6]; /* recomp-ui standard button/axis encoding */
    char gamepad_guid[40];
    char bios_path[512];
    char disc_path[512];
    uint32_t gamepad_instance; /* live SDL ID + 1; deliberately never persisted */
    float mouse_sensitivity;
    int mouse_invert_x, mouse_invert_y;
    int mouse_bind[3];
} CdiPlayerConfig;

void cdi_player_config_defaults(CdiPlayerConfig *config);

/* Load/save an explicit path. Load returns 1 on success, 0 when the file does
 * not exist, and -1 for malformed or unreadable input. */
int cdi_player_config_load(const char *path, CdiPlayerConfig *config);
int cdi_player_config_save(const char *path, const CdiPlayerConfig *config);

/* SDL supplies the per-user, cross-platform preference directory and creates
 * it when necessary. */
int cdi_player_config_default_path(char *path, size_t capacity);

/* Build a durable companion-file path beside player.cfg. This is shared by
 * the runtime and launcher rather than inferred from cwd. */
int cdi_player_config_sibling_path(const char *config_path,
                                   const char *filename,
                                   char *path, size_t capacity);
