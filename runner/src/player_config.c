#include "player_config.h"

#include <SDL.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *text) {
    char *end;
    while (isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

static int parse_bool(const char *text, int *value) {
    if (!strcmp(text, "true") || !strcmp(text, "1") ||
        !strcmp(text, "yes") || !strcmp(text, "on")) {
        *value = 1;
        return 1;
    }
    if (!strcmp(text, "false") || !strcmp(text, "0") ||
        !strcmp(text, "no") || !strcmp(text, "off")) {
        *value = 0;
        return 1;
    }
    return 0;
}

void cdi_player_config_defaults(CdiPlayerConfig *config) {
    memset(config, 0, sizeof *config);
    config->capture_mouse = 0;
    config->enable_mouse = 1;
    config->sync_host_on_startup = 0;
    config->fit_to_screen = 1;
    config->window_width = 1024;
    config->volume = 100;
    config->enable_audio = 1;
    config->input_source = 1;
    config->mouse_sensitivity = 0.06f;
    config->mouse_bind[0] = 4;
    config->mouse_bind[1] = 5;
    config->mouse_bind[2] = -1;
    const int keys[6] = {80, 82, 79, 81, 29, 27};
    const int pads[6] = {14, 12, 15, 13, 1, 2};
    memcpy(config->key_bind, keys, sizeof keys);
    memcpy(config->pad_bind, pads, sizeof pads);
}

static int parse_integer(const char *value, int minimum, int maximum, int *out) {
    char *end;
    errno = 0;
    long number = strtol(value, &end, 10);
    if (errno || end == value || *end || number < minimum || number > maximum) return 0;
    *out = (int)number;
    return 1;
}

int cdi_player_config_load(const char *path, CdiPlayerConfig *config) {
    FILE *file;
    char line[512];
    char section[32] = "";
    unsigned line_number = 0;

    cdi_player_config_defaults(config);
    errno = 0;
    file = fopen(path, "r");
    if (!file) return errno == ENOENT ? 0 : -1;

    while (fgets(line, sizeof line, file)) {
        char *key;
        char *value;
        char *equals;
        char *comment;
        line_number++;
        key = trim(line);
        if (!*key || *key == '#' || *key == ';') continue;
        if (*key == '[') {
            char *close = strchr(key + 1, ']');
            if (!close || *trim(close + 1)) goto malformed;
            *close = '\0';
            key = trim(key + 1);
            if (!*key || strlen(key) >= sizeof section) goto malformed;
            strcpy(section, key);
            continue;
        }
        equals = strchr(key, '=');
        if (!equals) goto malformed;
        *equals = '\0';
        value = trim(equals + 1);
        comment = strcmp(section,"assets") ? strpbrk(value, "#;") : NULL;
        if (comment) *comment = '\0';
        key = trim(key);
        value = trim(value);

        if (!strcmp(section, "input") && !strcmp(key, "capture_mouse")) {
            if (!parse_bool(value, &config->capture_mouse)) goto malformed;
        } else if (!strcmp(section, "input") && !strcmp(key, "enable_mouse")) {
            if (!parse_bool(value, &config->enable_mouse)) goto malformed;
        } else if (!strcmp(section, "rtc") &&
                   !strcmp(key, "sync_host_on_startup")) {
            if (!parse_bool(value, &config->sync_host_on_startup)) goto malformed;
        } else if (!strcmp(section, "launcher") && !strcmp(key, "skip_on_boot")) {
            if (!parse_bool(value, &config->skip_launcher)) goto malformed;
        } else if (!strcmp(section, "display")) {
            if (!strcmp(key, "fit_to_screen")) {
                if (!parse_bool(value, &config->fit_to_screen)) goto malformed;
            } else if (!strcmp(key, "window_width")) {
                if (!parse_integer(value, 320, 7680, &config->window_width)) goto malformed;
            } else if (!strcmp(key, "fullscreen")) {
                if (!parse_integer(value, 0, 2, &config->fullscreen)) goto malformed;
            } else if (!strcmp(key, "linear_filter")) {
                if (!parse_bool(value, &config->linear_filter)) goto malformed;
            }
        } else if (!strcmp(section, "audio")) {
            if (!strcmp(key, "volume")) {
                if (!parse_integer(value, 0, 100, &config->volume)) goto malformed;
            } else if (!strcmp(key, "enabled")) {
                if (!parse_bool(value, &config->enable_audio)) goto malformed;
            }
        } else if (!strcmp(section, "input")) {
            if (!strcmp(key, "source")) {
                if (!parse_integer(value, 0, 2, &config->input_source)) goto malformed;
            } else if (!strcmp(key, "gamepad_guid")) {
                if (strlen(value) >= sizeof config->gamepad_guid) goto malformed;
                strcpy(config->gamepad_guid, value);
            } else if (!strncmp(key, "key", 3) && strlen(key) == 4 && key[3] >= '0' && key[3] <= '5') {
                if (!parse_integer(value, 0, 511, &config->key_bind[key[3]-'0'])) goto malformed;
            } else if (!strncmp(key, "pad", 3) && strlen(key) == 4 && key[3] >= '0' && key[3] <= '5') {
                if (!parse_integer(value, 0, 999, &config->pad_bind[key[3]-'0'])) goto malformed;
            }
        } else if (!strcmp(section, "mouse")) {
            if (!strcmp(key,"sensitivity")) {
                char *end;
                float sensitivity=strtof(value,&end);
                if(end==value || *end || !(sensitivity>=0.01f && sensitivity<=0.5f)) goto malformed;
                config->mouse_sensitivity=sensitivity;
            } else if (!strcmp(key,"invert_x")) {
                if(!parse_bool(value,&config->mouse_invert_x))goto malformed;
            } else if (!strcmp(key,"invert_y")) {
                if(!parse_bool(value,&config->mouse_invert_y))goto malformed;
            } else if (!strncmp(key,"bind",4) && strlen(key)==5 && key[4]>='0' && key[4]<='2') {
                if(!parse_integer(value,-1,5,&config->mouse_bind[key[4]-'0']))goto malformed;
            }
        } else if (!strcmp(section, "assets")) {
            char *destination = !strcmp(key, "bios") ? config->bios_path :
                                !strcmp(key, "disc") ? config->disc_path : NULL;
            if (destination) {
                if (strlen(value) >= sizeof config->bios_path) goto malformed;
                strcpy(destination, value);
            }
        }
    }
    if (ferror(file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return 1;

malformed:
    fprintf(stderr, "[config] malformed %s at line %u\n", path, line_number);
    fclose(file);
    cdi_player_config_defaults(config);
    return -1;
}

int cdi_player_config_save(const char *path, const CdiPlayerConfig *config) {
    FILE *file = fopen(path, "w");
    if (!file) return 0;
    fprintf(file,
            "# cdirecomp player preferences (persistent, not per-title)\n"
            "[input]\n"
            "capture_mouse = %s\n"
            "enable_mouse = %s\n\n"
            "[rtc]\n"
            "sync_host_on_startup = %s\n\n",
            config->capture_mouse ? "true" : "false",
            config->enable_mouse ? "true" : "false",
            config->sync_host_on_startup ? "true" : "false");
    fprintf(file, "[display]\nfit_to_screen = %s\nwindow_width = %d\nfullscreen = %d\nlinear_filter = %s\n\n"
                  "[audio]\nvolume = %d\nenabled = %s\n\n"
                  "[input]\nsource = %d\ngamepad_guid = %s\n",
            config->fit_to_screen ? "true" : "false", config->window_width,
            config->fullscreen, config->linear_filter ? "true" : "false",
            config->volume, config->enable_audio ? "true" : "false",
            config->input_source, config->gamepad_guid);
    for (int i=0; i<6; i++) fprintf(file, "key%d = %d\npad%d = %d\n", i,config->key_bind[i],i,config->pad_bind[i]);
    fprintf(file,"\n[mouse]\nsensitivity = %.6f\ninvert_x = %s\ninvert_y = %s\n",
            config->mouse_sensitivity,config->mouse_invert_x ? "true":"false",config->mouse_invert_y ? "true":"false");
    for(int i=0;i<3;i++)fprintf(file,"bind%d = %d\n",i,config->mouse_bind[i]);
    fprintf(file, "\n[launcher]\nskip_on_boot = %s\n\n[assets]\nbios = %s\ndisc = %s\n",
            config->skip_launcher ? "true" : "false", config->bios_path, config->disc_path);
    if (fclose(file) != 0) return 0;
    return 1;
}

int cdi_player_config_default_path(char *path, size_t capacity) {
    char *directory = SDL_GetPrefPath("cdirecomp", "player");
    int written;
    if (!directory) return 0;
    written = snprintf(path, capacity, "%splayer.cfg", directory);
    SDL_free(directory);
    return written > 0 && (size_t)written < capacity;
}

int cdi_player_config_sibling_path(const char *config_path,
                                   const char *filename,
                                   char *path, size_t capacity) {
    const char *slash = strrchr(config_path, '/');
    const char *backslash = strrchr(config_path, '\\');
    const char *separator = slash;
    size_t directory_length;
    size_t filename_length = strlen(filename);
    if (!separator || (backslash && backslash > separator)) separator = backslash;
    directory_length = separator ? (size_t)(separator - config_path + 1) : 0;
    if (!filename_length || directory_length + filename_length + 1 > capacity)
        return 0;
    if (directory_length) memcpy(path, config_path, directory_length);
    memcpy(path + directory_length, filename, filename_length + 1);
    return 1;
}
