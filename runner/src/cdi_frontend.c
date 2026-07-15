#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "cdi_frontend.h"
#include "cdi_runtime.h"
#include "cdi_media.h"
#include "cdi_audio.h"
#include "mcd212_video.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_GameController *controller;
    SDL_JoystickID controller_id;
    SDL_AudioDeviceID audio_device;
    uint32_t *pixels;
    uint16_t texture_width, texture_height;
    uint64_t shown_generation;
    uint64_t next_event_ms;
    int fullscreen;
    int mouse_captured;
} Frontend;

static Frontend s;

static void close_controller(void) {
    if (s.controller) SDL_GameControllerClose(s.controller);
    s.controller = NULL;
    s.controller_id = -1;
}

static void open_first_controller(void) {
    if (s.controller) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        s.controller = SDL_GameControllerOpen(i);
        if (s.controller) {
            SDL_Joystick *joy = SDL_GameControllerGetJoystick(s.controller);
            s.controller_id = SDL_JoystickInstanceID(joy);
            return;
        }
    }
}

static void set_fullscreen(int enabled) {
    Uint32 flags = enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
    if (SDL_SetWindowFullscreen(s.window, flags) == 0) s.fullscreen = enabled;
}

static int apply_mouse_capture(void) {
    int wanted = cdi_input_mouse_active();
    if (wanted == s.mouse_captured) return 1;
    if (wanted) {
        if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0) {
            fprintf(stderr, "[frontend] cannot capture relative mouse: %s\n",
                    SDL_GetError());
            return 0;
        }
        SDL_SetWindowGrab(s.window, SDL_TRUE);
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_SetWindowGrab(s.window, SDL_FALSE);
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
    }
    s.mouse_captured = wanted;
    return 1;
}

static uint32_t input_mask(void) {
    const Uint8 *key = SDL_GetKeyboardState(NULL);
    uint32_t mask = 0;
    if (key[SDL_SCANCODE_LEFT]  || key[SDL_SCANCODE_A]) mask |= CDI_INPUT_LEFT;
    if (key[SDL_SCANCODE_UP]    || key[SDL_SCANCODE_W]) mask |= CDI_INPUT_UP;
    if (key[SDL_SCANCODE_RIGHT] || key[SDL_SCANCODE_D]) mask |= CDI_INPUT_RIGHT;
    if (key[SDL_SCANCODE_DOWN]  || key[SDL_SCANCODE_S]) mask |= CDI_INPUT_DOWN;
    if (key[SDL_SCANCODE_RETURN] || key[SDL_SCANCODE_SPACE] ||
        key[SDL_SCANCODE_Z]) mask |= CDI_INPUT_BTN1;
    if (key[SDL_SCANCODE_BACKSPACE] || key[SDL_SCANCODE_X]) mask |= CDI_INPUT_BTN2;

    if (s.controller) {
        if (SDL_GameControllerGetButton(s.controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
            mask |= CDI_INPUT_LEFT;
        if (SDL_GameControllerGetButton(s.controller, SDL_CONTROLLER_BUTTON_DPAD_UP))
            mask |= CDI_INPUT_UP;
        if (SDL_GameControllerGetButton(s.controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
            mask |= CDI_INPUT_RIGHT;
        if (SDL_GameControllerGetButton(s.controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
            mask |= CDI_INPUT_DOWN;
        if (SDL_GameControllerGetButton(s.controller, SDL_CONTROLLER_BUTTON_A))
            mask |= CDI_INPUT_BTN1;
        if (SDL_GameControllerGetButton(s.controller, SDL_CONTROLLER_BUTTON_B))
            mask |= CDI_INPUT_BTN2;
    }
    return mask;
}

static int ensure_texture(uint16_t width, uint16_t height) {
    if (s.texture && width == s.texture_width && height == s.texture_height) return 1;
    if (s.texture) SDL_DestroyTexture(s.texture);
    s.texture = SDL_CreateTexture(s.renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!s.texture) {
        fprintf(stderr, "[frontend] cannot create %ux%u texture: %s\n",
                width, height, SDL_GetError());
        return 0;
    }
    SDL_SetTextureBlendMode(s.texture, SDL_BLENDMODE_NONE);
    s.texture_width = width;
    s.texture_height = height;
    return 1;
}

static int present_frame(void) {
    uint16_t width, height;
    uint64_t generation;
    uint32_t count = mcd212_video_copy_frame(NULL, 0, &width, &height, &generation);
    if (!generation || generation == s.shown_generation) return 1;
    if (!ensure_texture(width, height)) return 0;
    if (count > MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT) return 0;
    mcd212_video_copy_frame(s.pixels, MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT,
                            &width, &height, &generation);
    if (SDL_UpdateTexture(s.texture, NULL, s.pixels, (int)width * 4) != 0) {
        fprintf(stderr, "[frontend] texture update failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetRenderDrawColor(s.renderer, 0, 0, 0, 255);
    SDL_RenderClear(s.renderer);
    SDL_RenderCopy(s.renderer, s.texture, NULL, NULL);
    SDL_RenderPresent(s.renderer);
    s.shown_generation = generation;
    return 1;
}

static void pump_audio(void) {
    int16_t pcm[2048 * CDI_AUDIO_OUTPUT_CHANNELS];
    uint32_t queued_frames;
    uint32_t frames;
    if (!s.audio_device) return;
    queued_frames = SDL_GetQueuedAudioSize(s.audio_device) /
                    (sizeof(int16_t) * CDI_AUDIO_OUTPUT_CHANNELS);
    if (queued_frames >= CDI_AUDIO_OUTPUT_RATE / 4u) return;
    frames = cdi_audio_read_frames(pcm, 2048);
    if (frames && SDL_QueueAudio(s.audio_device, pcm,
                                frames * sizeof(int16_t) *
                                CDI_AUDIO_OUTPUT_CHANNELS) != 0)
        fprintf(stderr, "[frontend] audio queue failed: %s\n", SDL_GetError());
}

int cdi_frontend_init(int capture_mouse) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[frontend] SDL initialization failed: %s\n", SDL_GetError());
        return 0;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec desired;
        SDL_zero(desired);
        desired.freq = CDI_AUDIO_OUTPUT_RATE;
        desired.format = AUDIO_S16SYS;
        desired.channels = CDI_AUDIO_OUTPUT_CHANNELS;
        desired.samples = 1024;
        s.audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
        if (s.audio_device)
            SDL_PauseAudioDevice(s.audio_device, 0);
        else
            fprintf(stderr, "[frontend] audio output unavailable: %s\n",
                    SDL_GetError());
    } else {
        fprintf(stderr, "[frontend] audio subsystem unavailable: %s\n",
                SDL_GetError());
    }
    s.window = SDL_CreateWindow("cdirecomp — Philips CD-i",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                768, 576, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s.window) {
        fprintf(stderr, "[frontend] window creation failed: %s\n", SDL_GetError());
        cdi_frontend_shutdown();
        return 0;
    }
    s.renderer = SDL_CreateRenderer(s.window, -1, SDL_RENDERER_ACCELERATED);
    if (!s.renderer)
        s.renderer = SDL_CreateRenderer(s.window, -1, SDL_RENDERER_SOFTWARE);
    if (!s.renderer) {
        fprintf(stderr, "[frontend] renderer creation failed: %s\n", SDL_GetError());
        cdi_frontend_shutdown();
        return 0;
    }
    SDL_RenderSetLogicalSize(s.renderer, 768, 576);
    SDL_RenderSetIntegerScale(s.renderer, SDL_FALSE);
    s.pixels = (uint32_t *)malloc((size_t)MCD212_VIDEO_MAX_WIDTH *
                                  MCD212_VIDEO_MAX_HEIGHT * sizeof(uint32_t));
    if (!s.pixels) {
        fprintf(stderr, "[frontend] framebuffer allocation failed\n");
        cdi_frontend_shutdown();
        return 0;
    }
    s.controller_id = -1;
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    open_first_controller();
    cdi_input_mouse_configure(capture_mouse);
    cdi_input_mouse_focus(
        (SDL_GetWindowFlags(s.window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    if (!apply_mouse_capture()) {
        cdi_frontend_shutdown();
        return 0;
    }
    s.next_event_ms = SDL_GetTicks64();
    return 1;
}

int cdi_frontend_pump(void) {
    uint64_t now = SDL_GetTicks64();
    if (now >= s.next_event_ms) {
        SDL_Event event;
        s.next_event_ms = now + 4;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) return 0;
            if (event.type == SDL_WINDOWEVENT &&
                (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                 event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
                cdi_input_mouse_focus(
                    event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED);
                if (!apply_mouse_capture()) return 0;
            } else if (event.type == SDL_MOUSEMOTION) {
                cdi_input_mouse_motion(event.motion.xrel, event.motion.yrel);
            } else if (event.type == SDL_MOUSEBUTTONDOWN ||
                       event.type == SDL_MOUSEBUTTONUP) {
                int button = event.button.button == SDL_BUTTON_LEFT ? 1 :
                             event.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
                cdi_input_mouse_button(
                    button, event.type == SDL_MOUSEBUTTONDOWN);
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) return 0;
                if (event.key.keysym.scancode == SDL_SCANCODE_F11 ||
                    (event.key.keysym.scancode == SDL_SCANCODE_RETURN &&
                     (event.key.keysym.mod & KMOD_ALT)))
                    set_fullscreen(!s.fullscreen);
            } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
                open_first_controller();
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED &&
                       event.cdevice.which == s.controller_id) {
                close_controller();
                open_first_controller();
            } else if (event.type == SDL_DROPFILE) {
                if (cdi_media_mount(event.drop.file))
                    SDL_SetWindowTitle(s.window, "cdirecomp — Philips CD-i — disc inserted");
                SDL_free(event.drop.file);
            }
        }
        cdi_input_set(input_mask());
    }
    pump_audio();
    return present_frame();
}

void cdi_frontend_shutdown(void) {
    cdi_input_set(0);
    cdi_input_mouse_focus(0);
    apply_mouse_capture();
    cdi_input_mouse_configure(0);
    close_controller();
    if (s.audio_device) SDL_CloseAudioDevice(s.audio_device);
    free(s.pixels);
    if (s.texture) SDL_DestroyTexture(s.texture);
    if (s.renderer) SDL_DestroyRenderer(s.renderer);
    if (s.window) SDL_DestroyWindow(s.window);
    SDL_Quit();
    s = (Frontend){0};
}
