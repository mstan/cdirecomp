#pragma once
#include <stdint.h>
struct SDL_Renderer;
struct SDL_Texture;
/* Context-thread timestamp queries and native-size hardware readback only.
 * Debugger reads cached telemetry; it never touches a foreign GL context. */
int cdi_render_profile_init(struct SDL_Renderer *,struct SDL_Texture *);
int cdi_render_profile_enabled(void);
int cdi_render_profile_wants_readback(uint64_t generation);
void cdi_render_profile_frame(void);
void cdi_render_profile_begin(unsigned pass);
void cdi_render_profile_end(unsigned pass);
void cdi_render_profile_upload(uint64_t bytes);
void cdi_render_profile_readback(struct SDL_Renderer *,const uint32_t *,unsigned,unsigned,uint64_t);
void cdi_render_profile_finish(void);
void cdi_render_profile_state(char *out,int capacity);
void cdi_render_profile_shutdown(void);
void cdi_render_profile_context_lost(void);
