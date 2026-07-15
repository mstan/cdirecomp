#include "cdi_runtime.h"

#include <stdio.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

int main(void) {
    int x, y;

    cdi_input_reset();
    cdi_input_set(CDI_INPUT_LEFT);
    cdi_input_mouse_configure(1);
    CHECK(!cdi_input_mouse_active());
    cdi_input_mouse_motion(10, 20);
    cdi_input_mouse_button(1, 1);
    cdi_input_pending_relative(&x, &y);
    CHECK(x == 0 && y == 0);
    CHECK(cdi_input_get() == CDI_INPUT_LEFT);

    cdi_input_mouse_focus(1);
    CHECK(cdi_input_mouse_active());
    cdi_input_mouse_motion(300, -300);
    cdi_input_mouse_button(1, 1);
    CHECK(cdi_input_get() == (CDI_INPUT_LEFT | CDI_INPUT_BTN1));
    cdi_input_acknowledge_mouse_buttons();
    cdi_input_mouse_button(2, 1);
    CHECK(cdi_input_get() ==
          (CDI_INPUT_LEFT | CDI_INPUT_BTN1 | CDI_INPUT_BTN2));
    cdi_input_acknowledge_mouse_buttons();

    cdi_input_take_relative(&x, &y, -128, 127, -128, 127);
    CHECK(x == 127 && y == -128);
    cdi_input_pending_relative(&x, &y);
    CHECK(x == 173 && y == -172);
    cdi_input_take_relative(&x, &y, -128, 127, -128, 127);
    CHECK(x == 127 && y == -128);
    cdi_input_take_relative(&x, &y, -128, 127, -128, 127);
    CHECK(x == 46 && y == -44);
    cdi_input_pending_relative(&x, &y);
    CHECK(x == 0 && y == 0);

    cdi_input_mouse_button(1, 0);
    CHECK(cdi_input_get() == (CDI_INPUT_LEFT | CDI_INPUT_BTN2));
    cdi_input_acknowledge_mouse_buttons();
    cdi_input_mouse_motion(12, 34);
    cdi_input_mouse_focus(0);
    CHECK(!cdi_input_mouse_active());
    CHECK(cdi_input_get() == CDI_INPUT_LEFT);
    cdi_input_pending_relative(&x, &y);
    CHECK(x == 0 && y == 0);

    cdi_input_mouse_configure(0);

    /* A complete click inside one 25 ms interval remains two ordered states. */
    cdi_input_reset();
    cdi_input_mouse_configure(1);
    cdi_input_mouse_focus(1);
    cdi_input_mouse_button(1, 1);
    cdi_input_mouse_button(1, 0);
    CHECK(cdi_input_get() == CDI_INPUT_BTN1);
    cdi_input_acknowledge_mouse_buttons();
    CHECK(cdi_input_get() == 0);
    cdi_input_acknowledge_mouse_buttons();
    if (failures) return 1;
    puts("CD-i input tests passed");
    return 0;
}
