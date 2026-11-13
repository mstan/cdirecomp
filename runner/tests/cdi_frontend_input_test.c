/* Runs against production objects with SDL's dummy video/audio drivers.
 * SDL events exercise the native routing, without desktop input or a ROM. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "cdi_frontend.h"
#include "cdi_input.h"
#include "cdi_runtime.h"
#include "mcd212_video.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); failures++; } } while (0)

static void focus(Uint8 kind) {
    SDL_Event event = {0};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = 1;
    event.window.event = kind;
    CHECK(SDL_PushEvent(&event) == 1);
}
static void button(Uint32 type, int x, int y) {
    SDL_Event event = {0};
    event.type = type;
    event.button.windowID = 1;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = x; event.button.y = y;
    CHECK(SDL_PushEvent(&event) == 1);
}
static void pump(void) {
    SDL_Delay(5);
    CHECK(cdi_frontend_pump());
}
int main(int argc,char **argv) {
    CdiPlayerConfig config;
    SDL_setenv("SDL_VIDEODRIVER","dummy",1);
    SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    SDL_setenv("SDL_RENDER_DRIVER","software",1);
    cdi_player_config_defaults(&config);
    config.window_width = 960;
    CHECK(config.enable_mouse && !config.capture_mouse);
    if(argc>1 && !strcmp(argv[1],"--original"))config.fit_to_screen=0;
    if(argc>1 && !strcmp(argv[1],"--disabled"))config.enable_mouse=0;
    cdi_input_reset();
    mcd212_video_reset();
    mcd212_video_control(0, 0xCD000000u | (50u << 12) | 100u);
    mcd212_video_control(0, 0xCE800000u | 8u);
    mcd212_video_begin_frame(384,240,0);
    mcd212_video_end_frame();
    cdi_frontend_configure(&config);
    CHECK(cdi_frontend_init(config.capture_mouse));
    pump(); /* publish geometry before accepting pointer events */
    CHECK(!strcmp(SDL_GetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH),"1"));
    focus(SDL_WINDOWEVENT_FOCUS_GAINED);
    SDL_Event stale = {0};
    stale.type=SDL_WINDOWEVENT;
    stale.window.windowID=777;
    stale.window.event=SDL_WINDOWEVENT_FOCUS_LOST;
    CHECK(SDL_PushEvent(&stale)==1);
    /* Complete focus + press + release in one poll. Windowed coordinates
     * (788,372) map to the shell's (630,165), without mouse capture. */
    int pointer_y=config.fit_to_screen ? 372 : 496;
    button(SDL_MOUSEBUTTONDOWN,788,pointer_y);
    button(SDL_MOUSEBUTTONUP,788,pointer_y);
    pump();
    if(!config.enable_mouse) {
        int x,y;
        CHECK(!cdi_input_mouse_active());
        CHECK(cdi_input_get()==0);
        cdi_input_pending_relative(&x,&y);
        CHECK(!x && !y);
        cdi_frontend_shutdown();
        return failures ? 1 : 0;
    }
    CHECK(cdi_input_mouse_active());
    CHECK(!SDL_GetRelativeMouseMode());
    int x,y,dx,dy,total_x=0,total_y=0;
    cdi_input_pending_relative(&x,&y);
    CHECK(x==530 && y==115);
    CHECK(cdi_input_get()==0); /* click waits for bounded IKAT movement */
    do {
        cdi_input_acknowledge_mouse_buttons();
        cdi_input_take_relative(&dx,&dy,-128,127,-128,127);
        total_x+=dx; total_y+=dy;
        cdi_input_pending_relative(&x,&y);
    } while (x || y);
    CHECK(total_x==530 && total_y==115);
    CHECK(cdi_input_get()==CDI_INPUT_BTN1);
    cdi_input_acknowledge_mouse_buttons();
    CHECK(cdi_input_get()==0);
    cdi_input_acknowledge_mouse_buttons();
    focus(SDL_WINDOWEVENT_FOCUS_LOST);
    button(SDL_MOUSEBUTTONDOWN,788,pointer_y);
    pump();
    CHECK(!cdi_input_mouse_active());
    CHECK(cdi_input_get()==0);
    cdi_input_pending_relative(&x,&y);
    CHECK(x==0 && y==0);
    cdi_frontend_shutdown();
    if(failures)return 1;
    puts("Windowed SDL focus/click routing PASS (capture off, ordered edges)");
    return 0;
}
