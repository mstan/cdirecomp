#include "cdi_runtime.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>

static atomic_uint base_input;
enum { MOUSE_BUTTON_QUEUE_CAPACITY = 256 };

static atomic_uint mouse_physical_buttons;
static uint32_t mouse_button_queue[MOUSE_BUTTON_QUEUE_CAPACITY];
static atomic_uint mouse_button_read;
static atomic_uint mouse_button_write;
static atomic_int relative_x;
static atomic_int relative_y;
static atomic_int mouse_enabled;
static atomic_int mouse_focused;

static const uint32_t allowed_input =
    CDI_INPUT_LEFT | CDI_INPUT_UP | CDI_INPUT_RIGHT | CDI_INPUT_DOWN |
    CDI_INPUT_BTN1 | CDI_INPUT_BTN2;

static void saturating_add(atomic_int *target, int delta) {
    int before = atomic_load_explicit(target, memory_order_relaxed);
    for (;;) {
        int after;
        if (delta > 0 && before > INT_MAX - delta) after = INT_MAX;
        else if (delta < 0 && before < INT_MIN - delta) after = INT_MIN;
        else after = before + delta;
        if (atomic_compare_exchange_weak_explicit(
                target, &before, after, memory_order_release,
                memory_order_relaxed))
            return;
    }
}

static int take_axis(atomic_int *target, int low, int high) {
    int before = atomic_load_explicit(target, memory_order_acquire);
    for (;;) {
        int amount = before < low ? low : before > high ? high : before;
        int after = before - amount;
        if (atomic_compare_exchange_weak_explicit(
                target, &before, after, memory_order_acq_rel,
                memory_order_acquire))
            return amount;
    }
}

void cdi_input_reset(void) {
    atomic_store_explicit(&base_input, 0, memory_order_relaxed);
    atomic_store_explicit(&mouse_physical_buttons, 0, memory_order_relaxed);
    atomic_store_explicit(&mouse_button_read, 0, memory_order_relaxed);
    atomic_store_explicit(&mouse_button_write, 0, memory_order_relaxed);
    atomic_store_explicit(&relative_x, 0, memory_order_relaxed);
    atomic_store_explicit(&relative_y, 0, memory_order_relaxed);
    atomic_store_explicit(&mouse_enabled, 0, memory_order_relaxed);
    atomic_store_explicit(&mouse_focused, 0, memory_order_relaxed);
}

void cdi_input_set(uint32_t mask) {
    atomic_store_explicit(&base_input, mask & allowed_input,
                          memory_order_release);
}

uint32_t cdi_input_get(void) {
    uint32_t result = atomic_load_explicit(&base_input, memory_order_acquire);
    unsigned read = atomic_load_explicit(&mouse_button_read, memory_order_acquire);
    unsigned write = atomic_load_explicit(&mouse_button_write, memory_order_acquire);
    if (read != write)
        result |= mouse_button_queue[read % MOUSE_BUTTON_QUEUE_CAPACITY];
    else
        result |= atomic_load_explicit(&mouse_physical_buttons,
                                       memory_order_acquire);
    return result & allowed_input;
}

void cdi_input_add_relative(int x, int y) {
    saturating_add(&relative_x, x);
    saturating_add(&relative_y, y);
}

void cdi_input_clear_relative(void) {
    atomic_store_explicit(&relative_x, 0, memory_order_release);
    atomic_store_explicit(&relative_y, 0, memory_order_release);
}

void cdi_input_take_relative(int *x, int *y,
                             int min_x, int max_x,
                             int min_y, int max_y) {
    *x = take_axis(&relative_x, min_x, max_x);
    *y = take_axis(&relative_y, min_y, max_y);
}

void cdi_input_pending_relative(int *x, int *y) {
    *x = atomic_load_explicit(&relative_x, memory_order_acquire);
    *y = atomic_load_explicit(&relative_y, memory_order_acquire);
}

int cdi_input_mouse_active(void) {
    return atomic_load_explicit(&mouse_enabled, memory_order_acquire) &&
           atomic_load_explicit(&mouse_focused, memory_order_acquire);
}

static void release_mouse_state(void) {
    if (atomic_load_explicit(&mouse_physical_buttons, memory_order_acquire)) {
        cdi_input_mouse_button(1, 0);
        cdi_input_mouse_button(2, 0);
    }
    cdi_input_clear_relative();
}

void cdi_input_mouse_configure(int enabled) {
    atomic_store_explicit(&mouse_enabled, enabled != 0, memory_order_release);
    if (!enabled) release_mouse_state();
}

void cdi_input_mouse_focus(int focused) {
    atomic_store_explicit(&mouse_focused, focused != 0, memory_order_release);
    if (!focused) release_mouse_state();
}

void cdi_input_mouse_motion(int x, int y) {
    if (cdi_input_mouse_active()) cdi_input_add_relative(x, y);
}

void cdi_input_mouse_button(int button, int pressed) {
    uint32_t bit = button == 1 ? CDI_INPUT_BTN1 :
                   button == 2 ? CDI_INPUT_BTN2 : 0;
    uint32_t before;
    uint32_t after;
    unsigned read;
    unsigned write;
    if (!bit) return;
    if (pressed && !cdi_input_mouse_active()) return;
    before = atomic_load_explicit(&mouse_physical_buttons, memory_order_acquire);
    for (;;) {
        after = pressed ? before | bit : before & ~bit;
        if (after == before) return;
        if (atomic_compare_exchange_weak_explicit(
                &mouse_physical_buttons, &before, after, memory_order_acq_rel,
                memory_order_acquire))
            break;
    }
    write = atomic_load_explicit(&mouse_button_write, memory_order_relaxed);
    read = atomic_load_explicit(&mouse_button_read, memory_order_acquire);
    if (write - read >= MOUSE_BUTTON_QUEUE_CAPACITY) return;
    mouse_button_queue[write % MOUSE_BUTTON_QUEUE_CAPACITY] = after;
    atomic_store_explicit(&mouse_button_write, write + 1, memory_order_release);
}

void cdi_input_acknowledge_mouse_buttons(void) {
    unsigned read = atomic_load_explicit(&mouse_button_read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(&mouse_button_write, memory_order_acquire);
    if (read != write)
        atomic_store_explicit(&mouse_button_read, read + 1, memory_order_release);
}
