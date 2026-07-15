#include "player_config.h"

#include <stdio.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

int main(void) {
    const char *path = "player_config_test.cfg";
    CdiPlayerConfig written;
    CdiPlayerConfig loaded;
    FILE *file;

    remove(path);
    loaded.capture_mouse = 1;
    loaded.sync_host_on_startup = 1;
    CHECK(cdi_player_config_load(path, &loaded) == 0);
    CHECK(loaded.capture_mouse == 0);
    CHECK(loaded.sync_host_on_startup == 0);

    written.capture_mouse = 1;
    written.sync_host_on_startup = 1;
    CHECK(cdi_player_config_save(path, &written));
    CHECK(cdi_player_config_load(path, &loaded) == 1);
    CHECK(loaded.capture_mouse == 1);
    CHECK(loaded.sync_host_on_startup == 1);

    written.capture_mouse = 0;
    written.sync_host_on_startup = 0;
    CHECK(cdi_player_config_save(path, &written));
    CHECK(cdi_player_config_load(path, &loaded) == 1);
    CHECK(loaded.capture_mouse == 0);
    CHECK(loaded.sync_host_on_startup == 0);

    file = fopen(path, "w");
    CHECK(file != NULL);
    if (file) {
        fputs("[input]\ncapture_mouse = perhaps\n", file);
        fclose(file);
    }
    CHECK(cdi_player_config_load(path, &loaded) == -1);
    CHECK(loaded.capture_mouse == 0);
    CHECK(loaded.sync_host_on_startup == 0);

    remove(path);
    if (failures) return 1;
    puts("player config tests passed");
    return 0;
}
