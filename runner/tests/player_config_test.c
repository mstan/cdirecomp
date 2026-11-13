#include "player_config.h"

#include <stdio.h>
#include <string.h>

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
    char sibling[128];
    cdi_player_config_defaults(&written);

    remove(path);
    loaded.capture_mouse = 1;
    loaded.sync_host_on_startup = 1;
    CHECK(cdi_player_config_load(path, &loaded) == 0);
    CHECK(loaded.capture_mouse == 0);
    CHECK(loaded.sync_host_on_startup == 0);
    CHECK(loaded.fit_to_screen == 1);
    CHECK(loaded.volume == 100);
    CHECK(loaded.skip_launcher == 0);

    written.capture_mouse = 1;
    written.sync_host_on_startup = 1;
    written.fit_to_screen = 0;
    written.volume = 37;
    written.skip_launcher = 1;
    written.key_bind[4] = 44;
    written.mouse_bind[0] = 5;
    written.mouse_invert_y = 1;
    strcpy(written.disc_path, "D:/Games/Hotel Mario (USA).cue");
    CHECK(cdi_player_config_save(path, &written));
    CHECK(cdi_player_config_load(path, &loaded) == 1);
    CHECK(loaded.capture_mouse == 1);
    CHECK(loaded.sync_host_on_startup == 1);
    CHECK(loaded.fit_to_screen == 0);
    CHECK(loaded.volume == 37);
    CHECK(loaded.skip_launcher == 1);
    CHECK(loaded.key_bind[4] == 44);
    CHECK(loaded.mouse_bind[0] == 5);
    CHECK(loaded.mouse_invert_y == 1);
    CHECK(!strcmp(loaded.disc_path, written.disc_path));

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

    CHECK(cdi_player_config_sibling_path(
        "C:\\Users\\Player\\player.cfg", "nvram.bin",
        sibling, sizeof sibling));
    CHECK(!strcmp(sibling, "C:\\Users\\Player\\nvram.bin"));
    CHECK(cdi_player_config_sibling_path(
        "/tmp/cdirecomp/player.cfg", "nvram.bin", sibling, sizeof sibling));
    CHECK(!strcmp(sibling, "/tmp/cdirecomp/nvram.bin"));
    CHECK(cdi_player_config_sibling_path(
        "player.cfg", "nvram.bin", sibling, sizeof sibling));
    CHECK(!strcmp(sibling, "nvram.bin"));
    CHECK(!cdi_player_config_sibling_path(
        "player.cfg", "nvram.bin", sibling, 4));

    remove(path);
    if (failures) return 1;
    puts("player config tests passed");
    return 0;
}
