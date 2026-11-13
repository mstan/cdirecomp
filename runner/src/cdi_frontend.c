#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "cdi_frontend.h"
#include "cdi_runtime.h"
#include "cdi_input.h"
#include "cdi_media.h"
#include "cdi_audio.h"
#include "mcd212_video.h"
#include "cdi_render_profile.h"
#include "cdi_raster_jobs.h"
#include "cdi_gpu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef CDI_GPU_HLE
#define CDI_GPU_HLE 0
#endif

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Texture *native_target;
    SDL_Texture *gpu_presented;
    SDL_GameController *controller;
    SDL_JoystickID controller_id;
    SDL_AudioDeviceID audio_device;
    uint32_t *pixels;
    uint16_t texture_width, texture_height;
    uint64_t shown_generation;
    uint64_t shown_content;
    uint64_t shown_epoch;
    uint64_t next_event_ms;
    int fullscreen;
    int mouse_captured;
    uint32_t mouse_buttons;
    float mouse_x_fraction, mouse_y_fraction;
    unsigned scale;
    int redraw;
} Frontend;

#ifndef CDI_PRODUCT_NAME
#define CDI_PRODUCT_NAME "Philips CD-i"
#endif

static Frontend s;
static CdiPlayerConfig options;
static int options_ready;
void cdi_frontend_configure(const CdiPlayerConfig *config) {
    options = *config;
    options_ready = 1;
}

static void close_controller(void) {
    if (s.controller) SDL_GameControllerClose(s.controller);
    s.controller = NULL;
    s.controller_id = -1;
}

static void open_first_controller(void) {
    if(s.controller || options.input_source!=2)return;
    int selected=-1,candidates=0,exact=0;
    for(int i=0;i<SDL_NumJoysticks();i++) {
        if(!SDL_IsGameController(i))continue;
        char guid[40];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),guid,sizeof guid);
        if(options.gamepad_guid[0] && strcmp(guid,options.gamepad_guid))continue;
        if(options.gamepad_instance && (uint32_t)SDL_JoystickGetDeviceInstanceID(i)+1u==options.gamepad_instance) {
            selected=i;exact=1;break;
        }
        selected=i;candidates++;
    }
    if(selected<0 || (!exact && candidates!=1))return;
    s.controller=SDL_GameControllerOpen(selected);
    if(s.controller)s.controller_id=SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s.controller));
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
    if (options.input_source == 1) {
        const int defaults[6]={80,82,79,81,29,27};
        const int extras[6]={SDL_SCANCODE_A,SDL_SCANCODE_W,SDL_SCANCODE_D,SDL_SCANCODE_S,SDL_SCANCODE_RETURN,SDL_SCANCODE_BACKSPACE};
        for (int i=0;i<6;i++) {
            int code=options.key_bind[i];
            if ((code>0 && code<SDL_NUM_SCANCODES && key[code]) ||
                (code==defaults[i] && (key[extras[i]] || (i==4 && key[SDL_SCANCODE_SPACE])))) mask |= 1u<<i;
        }
    }
    if (s.controller && options.input_source == 2) {
        for (int i=0;i<6;i++) {
            int binding=options.pad_bind[i],pressed=0;
            if (binding>0 && binding<100 && binding-1<SDL_CONTROLLER_BUTTON_MAX)
                pressed=SDL_GameControllerGetButton(s.controller,(SDL_GameControllerButton)(binding-1));
            else if(binding>=100 && binding<1000 && (binding-100)/2<SDL_CONTROLLER_AXIS_MAX) {
                int axis=SDL_GameControllerGetAxis(s.controller,(SDL_GameControllerAxis)((binding-100)/2));
                pressed=((binding-100)&1) ? axis>16000 : axis < -16000;
            }
            if(pressed)mask |= 1u<<i;
        }
    }
    if(options.input_source==1 && cdi_input_mouse_active())mask |= s.mouse_buttons;
    return mask;
}

static int ensure_texture(uint16_t width, uint16_t height) {
    if (s.texture && width == s.texture_width && height == s.texture_height) return 1;
    if (s.texture) SDL_DestroyTexture(s.texture);
    if (s.native_target){SDL_DestroyTexture(s.native_target);s.native_target=NULL;}
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
    if(cdi_render_profile_init(s.renderer,s.texture)) {
        /* An actual native-size offscreen draw keeps hardware qualification
         * meaningful even for hidden windows. Both measured backends use this
         * same presentation boundary; no enhanced-size host buffer exists. */
        s.native_target=SDL_CreateTexture(s.renderer,SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_TARGET,width,height);
        if(!s.native_target)return 0;
        SDL_SetTextureBlendMode(s.native_target,SDL_BLENDMODE_NONE);
    }
#if CDI_GPU_HLE
    cdi_gpu_init(s.renderer,s.texture);
#endif
    return 1;
}

static int present_frame(void) {
    uint16_t width, height;
    uint64_t generation, content;
#if CDI_GPU_HLE
    if(s.shown_epoch!=cdi_raster_epoch()) {
        s.redraw=1;s.shown_content=0;s.gpu_presented=NULL;s.shown_epoch=cdi_raster_epoch();
    }
#endif
    uint32_t count = mcd212_video_copy_changed_frame(NULL,
        MCD212_VIDEO_MAX_WIDTH * MCD212_VIDEO_MAX_HEIGHT, s.shown_content,
        &width, &height, &generation, &content);
    if (!generation || (generation == s.shown_generation && !s.redraw)) return 1;
    if(!ensure_texture(width,height))return 0;
    cdi_render_profile_frame();
    SDL_Texture *presented=NULL;
    int portable_dirty=count || s.redraw;
#if CDI_GPU_HLE
    const CdiRasterFrame *frame=cdi_raster_acquire();
    if(frame && frame->generation==generation && frame->width==width && frame->height==height) {
        if(count || s.redraw || !s.gpu_presented || cdi_render_profile_wants_readback(generation)) {
            const uint32_t *check=NULL;
            if(cdi_render_profile_wants_readback(generation)) {
                mcd212_video_copy_frame(s.pixels,MCD212_VIDEO_MAX_WIDTH*MCD212_VIDEO_MAX_HEIGHT,NULL,NULL,NULL);check=s.pixels;
            }
            s.gpu_presented=cdi_gpu_render(frame,s.scale,check);
            if(!s.gpu_presented)portable_dirty=1;
        }
        presented=s.gpu_presented;
        if(!presented && portable_dirty) {
            if(!cdi_raster_native(frame,s.pixels,MCD212_VIDEO_MAX_WIDTH*MCD212_VIDEO_MAX_HEIGHT)){cdi_raster_release(frame);return 0;}
        }
    } else if(portable_dirty) {
        mcd212_video_copy_frame(s.pixels,MCD212_VIDEO_MAX_WIDTH*MCD212_VIDEO_MAX_HEIGHT,NULL,NULL,NULL);
    }
    cdi_raster_release(frame);
    if(!presented)cdi_gpu_native_fallback(width,height,s.scale);
#else
    if(portable_dirty)mcd212_video_copy_frame(s.pixels,MCD212_VIDEO_MAX_WIDTH*MCD212_VIDEO_MAX_HEIGHT,NULL,NULL,NULL);
#endif
    if (!presented && portable_dirty) {
        cdi_render_profile_begin(0);
        if (SDL_UpdateTexture(s.texture, NULL, s.pixels, (int)width * 4) != 0) {
            fprintf(stderr, "[frontend] texture update failed: %s\n", SDL_GetError());
            return 0;
        }
        if(cdi_render_profile_enabled())SDL_RenderFlush(s.renderer);
        cdi_render_profile_end(0);
        cdi_render_profile_upload((uint64_t)width*height*4);
    }
    s.shown_content = content;
    /* Keep field cadence and resize/expose redraws even when the existing
     * texture is unchanged. Only copying/uploading that texture is elided. */
    if(!presented)presented=s.texture;
    if(presented==s.texture && s.native_target) {
        if(SDL_SetRenderTarget(s.renderer,s.native_target))return 0;
        cdi_render_profile_begin(1);
        SDL_SetRenderDrawColor(s.renderer,0,0,0,255);SDL_RenderClear(s.renderer);
        SDL_RenderCopy(s.renderer,s.texture,NULL,NULL);SDL_RenderFlush(s.renderer);
        cdi_render_profile_end(1);
        cdi_render_profile_readback(s.renderer,s.pixels,width,height,generation);
        if(SDL_SetRenderTarget(s.renderer,NULL))return 0;
        presented=s.native_target;
    }
    cdi_render_profile_begin(2);
    SDL_SetRenderDrawColor(s.renderer, 0, 0, 0, 255);
    SDL_RenderClear(s.renderer);
    SDL_RenderCopy(s.renderer, presented, NULL, NULL);
    if(cdi_render_profile_enabled())SDL_RenderFlush(s.renderer);
    cdi_render_profile_end(2);
    SDL_RenderPresent(s.renderer);
    cdi_render_profile_finish();
    s.shown_generation = generation;
    s.redraw=0;
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
    int volume=options.enable_audio ? options.volume : 0;
    if(volume!=100)for(uint32_t i=0;i<frames*CDI_AUDIO_OUTPUT_CHANNELS;i++)
        pcm[i]=(int16_t)((int32_t)pcm[i]*volume/100);
    if (frames && SDL_QueueAudio(s.audio_device, pcm,
                                frames * sizeof(int16_t) *
                                CDI_AUDIO_OUTPUT_CHANNELS) != 0)
        fprintf(stderr, "[frontend] audio queue failed: %s\n", SDL_GetError());
}

int cdi_frontend_init(int capture_mouse) {
    if(!options_ready)cdi_player_config_defaults(&options);
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, options.linear_filter ? "linear" : "nearest");
#if CDI_GPU_HLE
    if(!getenv("SDL_RENDER_DRIVER"))SDL_SetHint(SDL_HINT_RENDER_DRIVER,"opengl");
#endif
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
    s.window = SDL_CreateWindow(CDI_PRODUCT_NAME,
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                options.window_width, options.window_width * (options.fit_to_screen ? 9 : 12) / 16, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                (getenv("CDI_WINDOW_HIDDEN")?SDL_WINDOW_HIDDEN:0));
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
    if(!options.fit_to_screen)SDL_RenderSetLogicalSize(s.renderer, 768, 576);
    SDL_RenderSetIntegerScale(s.renderer, SDL_FALSE);
    s.pixels = (uint32_t *)malloc((size_t)MCD212_VIDEO_MAX_WIDTH *
                                  MCD212_VIDEO_MAX_HEIGHT * sizeof(uint32_t));
    if (!s.pixels) {
        fprintf(stderr, "[frontend] framebuffer allocation failed\n");
        cdi_frontend_shutdown();
        return 0;
    }
    s.controller_id = -1;
    if(options.fullscreen)set_fullscreen(1);
    s.scale=1;
    if(getenv("CDI_INTERNAL_SCALE")) {
        unsigned value=(unsigned)strtoul(getenv("CDI_INTERNAL_SCALE"),NULL,10);
        if(value && value<=64 && !(value&(value-1u)))s.scale=value;
    }
    s.redraw=1;
#if CDI_GPU_HLE
    mcd212_video_capture_raster(1);
#endif
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    open_first_controller();
    cdi_input_mouse_configure(capture_mouse && options.input_source==1);
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
            if(event.type==SDL_RENDER_DEVICE_RESET || event.type==SDL_RENDER_TARGETS_RESET) {
                if(event.type==SDL_RENDER_DEVICE_RESET)cdi_render_profile_context_lost();
                cdi_gpu_reset(event.type==SDL_RENDER_DEVICE_RESET);s.gpu_presented=NULL;s.redraw=1;s.shown_content=0;
                if(s.texture){SDL_DestroyTexture(s.texture);s.texture=NULL;}
            }
            if(event.type==SDL_WINDOWEVENT && (event.window.event==SDL_WINDOWEVENT_EXPOSED || event.window.event==SDL_WINDOWEVENT_SIZE_CHANGED || event.window.event==SDL_WINDOWEVENT_RESTORED))s.redraw=1;
            if (event.type == SDL_WINDOWEVENT &&
                (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                 event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
                if(event.window.event==SDL_WINDOWEVENT_FOCUS_LOST)s.mouse_buttons=0;
                cdi_input_mouse_focus(
                    event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED);
                if (!apply_mouse_capture()) return 0;
            } else if (event.type == SDL_MOUSEMOTION) {
                float scale=options.mouse_sensitivity/0.06f;
                s.mouse_x_fraction+=event.motion.xrel*scale*(options.mouse_invert_x?-1.f:1.f);
                s.mouse_y_fraction+=event.motion.yrel*scale*(options.mouse_invert_y?-1.f:1.f);
                int dx=(int)s.mouse_x_fraction,dy=(int)s.mouse_y_fraction;
                s.mouse_x_fraction-=dx;s.mouse_y_fraction-=dy;
                cdi_input_mouse_motion(dx,dy);
            } else if (event.type == SDL_MOUSEBUTTONDOWN ||
                       event.type == SDL_MOUSEBUTTONUP) {
                int button=event.button.button==SDL_BUTTON_LEFT ? 0 :
                           event.button.button==SDL_BUTTON_RIGHT ? 1 :
                           event.button.button==SDL_BUTTON_MIDDLE ? 2 : -1;
                if(button>=0 && options.mouse_bind[button]>=0 && options.mouse_bind[button]<6) {
                    uint32_t mask=1u<<options.mouse_bind[button];
                    if(event.type==SDL_MOUSEBUTTONDOWN)s.mouse_buttons |= mask;
                    else s.mouse_buttons &= ~mask;
                }
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) return 0;
                if(event.key.keysym.scancode==SDL_SCANCODE_F8) {
                    s.scale=s.scale==64?1:s.scale*2;s.redraw=1;
                    fprintf(stderr,"[frontend] internal density %ux (F8 cycles1/2/4/8/16/32/64)\n",s.scale);
                }
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
                    SDL_SetWindowTitle(s.window, CDI_PRODUCT_NAME);
                SDL_free(event.drop.file);
            }
        }
        cdi_input_set_frontend(input_mask());
    }
    pump_audio();
    return present_frame();
}

void cdi_frontend_shutdown(void) {
    cdi_gpu_reset(0);
    mcd212_video_capture_raster(0);
    cdi_render_profile_shutdown();
    if(s.native_target)SDL_DestroyTexture(s.native_target);
    cdi_input_set_frontend(0);
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
