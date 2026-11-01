#pragma once
#include <stdint.h>
#include "cdi_raster_jobs.h"
struct SDL_Renderer;
struct SDL_Texture;
int cdi_gpu_init(struct SDL_Renderer *,struct SDL_Texture *activation);
struct SDL_Texture *cdi_gpu_render(const CdiRasterFrame *,unsigned scale,const uint32_t *native_check);
void cdi_gpu_native_fallback(unsigned width,unsigned height,unsigned requested);
void cdi_gpu_state(char *out,int capacity);
void cdi_gpu_reset(int context_lost);
