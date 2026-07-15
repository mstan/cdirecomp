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
    config->capture_mouse = 0;
    config->sync_host_on_startup = 0;
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
        comment = strpbrk(value, "#;");
        if (comment) *comment = '\0';
        key = trim(key);
        value = trim(value);

        if (!strcmp(section, "input") && !strcmp(key, "capture_mouse")) {
            if (!parse_bool(value, &config->capture_mouse)) goto malformed;
        } else if (!strcmp(section, "rtc") &&
                   !strcmp(key, "sync_host_on_startup")) {
            if (!parse_bool(value, &config->sync_host_on_startup)) goto malformed;
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
            "capture_mouse = %s\n\n"
            "[rtc]\n"
            "sync_host_on_startup = %s\n",
            config->capture_mouse ? "true" : "false",
            config->sync_host_on_startup ? "true" : "false");
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
