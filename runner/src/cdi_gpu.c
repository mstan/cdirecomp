/* Owned native scanout jobs on a resident GPU. No live guest memory is read.
 * All scales use the same authored 256x16 tiles and command count. The largest
 * scratch image is16384x1024 RGBA8 (64MiB), never a full S-squared frame. */
#include "cdi_gpu.h"
#include "cdi_render_profile.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef CDI_GPU_HLE
#define CDI_GPU_HLE 0
#endif
#if CDI_GPU_HLE && !defined(__EMSCRIPTEN__)
#include <SDL_opengl.h>

#define GL_API(X) \
 X(void,GetIntegerv,(GLenum,GLint *)) X(void,GetBooleanv,(GLenum,GLboolean *)) \
 X(GLboolean,IsEnabled,(GLenum)) X(GLenum,GetError,(void)) \
 X(void,Enable,(GLenum)) X(void,Disable,(GLenum)) X(void,ColorMask,(GLboolean,GLboolean,GLboolean,GLboolean)) \
 X(void,Viewport,(GLint,GLint,GLsizei,GLsizei)) X(void,Scissor,(GLint,GLint,GLsizei,GLsizei)) \
 X(void,PixelStorei,(GLenum,GLint)) X(void,ActiveTexture,(GLenum)) X(void,BindBuffer,(GLenum,GLuint)) \
 X(void,GenBuffers,(GLsizei,GLuint *)) X(void,DeleteBuffers,(GLsizei,const GLuint *)) \
 X(void,BufferData,(GLenum,GLsizeiptr,const void *,GLenum)) X(void,BufferSubData,(GLenum,GLintptr,GLsizeiptr,const void *)) \
 X(void,GenTextures,(GLsizei,GLuint *)) X(void,DeleteTextures,(GLsizei,const GLuint *)) \
 X(void,BindTexture,(GLenum,GLuint)) X(void,TexParameteri,(GLenum,GLenum,GLint)) \
 X(void,TexImage2D,(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void *)) \
 X(void,TexSubImage2D,(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void *)) \
 X(GLuint,CreateShader,(GLenum)) X(void,ShaderSource,(GLuint,GLsizei,const GLchar *const *,const GLint *)) \
 X(void,CompileShader,(GLuint)) X(void,GetShaderiv,(GLuint,GLenum,GLint *)) \
 X(void,GetShaderInfoLog,(GLuint,GLsizei,GLsizei *,GLchar *)) X(void,DeleteShader,(GLuint)) \
 X(GLuint,CreateProgram,(void)) X(void,AttachShader,(GLuint,GLuint)) X(void,LinkProgram,(GLuint)) \
 X(void,GetProgramiv,(GLuint,GLenum,GLint *)) X(void,GetProgramInfoLog,(GLuint,GLsizei,GLsizei *,GLchar *)) \
 X(void,DeleteProgram,(GLuint)) X(void,UseProgram,(GLuint)) X(GLint,GetUniformLocation,(GLuint,const GLchar *)) \
 X(void,Uniform1i,(GLint,GLint)) X(void,Uniform4i,(GLint,GLint,GLint,GLint,GLint)) \
 X(void,GenVertexArrays,(GLsizei,GLuint *)) X(void,DeleteVertexArrays,(GLsizei,const GLuint *)) \
 X(void,BindVertexArray,(GLuint)) X(void,MultiDrawArraysIndirect,(GLenum,const void *,GLsizei,GLsizei)) \
 X(void,BindFramebuffer,(GLenum,GLuint)) \
 X(void,GenFramebuffers,(GLsizei,GLuint *)) X(void,DeleteFramebuffers,(GLsizei,const GLuint *)) \
 X(void,FramebufferTexture2D,(GLenum,GLenum,GLenum,GLuint,GLint)) X(GLenum,CheckFramebufferStatus,(GLenum)) \
 X(void,BlitFramebuffer,(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum)) \
 X(void,ClearColor,(GLfloat,GLfloat,GLfloat,GLfloat)) X(void,Clear,(GLbitfield))
#define DECLARE(ret,name,args) typedef ret(APIENTRY *name##Fn)args;
GL_API(DECLARE)
#undef DECLARE
static struct {
#define MEMBER(ret,name,args) name##Fn name;
 GL_API(MEMBER)
#undef MEMBER
} gl;
static struct {
    SDL_Renderer *renderer;SDL_GLContext context;
    SDL_Texture *tile,*output,*check;
    GLuint program,vao,source,commands,output_fbo,check_fbo,tile_fbo;
    GLint region_location,limit;
    uint64_t rows[MCD212_VIDEO_MAX_HEIGHT],epoch,frames,uploads,upload_bytes,draws,resets;
    unsigned requested,effective,width,height,output_w,output_h,tile_w,tile_h,check_w,check_h;
    int checked,available,failed;
} gpu;
static const char *vertex_source=
    "#version 330 core\n"
    "uniform usampler2D source_jobs;uniform ivec4 region;flat out uvec3 data;\n"
    "void main(){int row=gl_VertexID/4,index=gl_InstanceID;"
    "uvec4 j=texelFetch(source_jobs,ivec2(index+1,row),0);"
    "int a=clamp(int(j.x&65535u)-region.x,0,region.z),b=clamp(int(j.x>>16u)-region.x,0,region.z);"
    "int x=(gl_VertexID&1)==0?a:b;int y=(gl_VertexID>>1)&1;"
    "gl_Position=vec4(float(x)*2.0/float(region.z)-1.0,float(row-region.y+y)*2.0/float(region.w)-1.0,0.0,1.0);data=j.yzw;}\n";
static const char *fragment_source=
    "#version 330 core\n"
    "flat in uvec3 data;out vec4 result;\n"
    "ivec3 rgb(uint v){return ivec3(int((v>>16u)&255u),int((v>>8u)&255u),int(v&255u));}\n"
    "ivec3 contribution(ivec3 c,int f){return clamp(f*(c-ivec3(16))/63+ivec3(16),ivec3(0),ivec3(255));}\n"
    "void main(){uint mode=data.z&3u;ivec3 c=rgb(data.x);float alpha=1.0;"
    "if(mode==0u)alpha=float(data.x>>24u)/255.0;"
    "else{c=contribution(c,int((data.z>>2u)&63u));"
    "if(mode==2u)c=clamp(c+contribution(rgb(data.y),int((data.z>>8u)&63u))-ivec3(16),ivec3(0),ivec3(255));}"
    "result=vec4(vec3(c)/255.0,alpha);}\n";
enum {SWITCHES=9,STORES=6};
static const GLenum switches[SWITCHES]={GL_BLEND,GL_SCISSOR_TEST,GL_DEPTH_TEST,GL_STENCIL_TEST,GL_CULL_FACE,GL_DITHER,GL_FRAMEBUFFER_SRGB,GL_RASTERIZER_DISCARD,GL_TEXTURE_2D};
static const GLenum stores[STORES]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_PIXELS,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SWAP_BYTES,GL_UNPACK_LSB_FIRST};
typedef struct {
    GLint program,active,texture,vao,unpack_buffer,indirect_buffer,read_fbo,draw_fbo,viewport[4],scissor[4],unpack[STORES];
    GLboolean mask[4],enabled[SWITCHES];
} GlState;
static void save_state(GlState *state) {
    gl.GetIntegerv(GL_CURRENT_PROGRAM,&state->program);gl.GetIntegerv(GL_ACTIVE_TEXTURE,&state->active);
    gl.ActiveTexture(GL_TEXTURE0);gl.GetIntegerv(GL_TEXTURE_BINDING_2D,&state->texture);
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING,&state->vao);gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&state->unpack_buffer);
    gl.GetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING,&state->indirect_buffer);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&state->read_fbo);gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&state->draw_fbo);
    gl.GetIntegerv(GL_VIEWPORT,state->viewport);gl.GetIntegerv(GL_SCISSOR_BOX,state->scissor);gl.GetBooleanv(GL_COLOR_WRITEMASK,state->mask);
    for(unsigned i=0;i<SWITCHES;i++){state->enabled[i]=gl.IsEnabled(switches[i]);gl.Disable(switches[i]);}
    for(unsigned i=0;i<STORES;i++)gl.GetIntegerv(stores[i],&state->unpack[i]);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);gl.PixelStorei(GL_UNPACK_ALIGNMENT,1);
    for(unsigned i=1;i<STORES;i++)gl.PixelStorei(stores[i],0);
    gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
}
static void restore_state(const GlState *state) {
    gl.UseProgram((GLuint)state->program);gl.BindVertexArray((GLuint)state->vao);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,(GLuint)state->unpack_buffer);
    gl.BindBuffer(GL_DRAW_INDIRECT_BUFFER,(GLuint)state->indirect_buffer);
    for(unsigned i=0;i<STORES;i++)gl.PixelStorei(stores[i],state->unpack[i]);
    gl.ActiveTexture(GL_TEXTURE0);gl.BindTexture(GL_TEXTURE_2D,(GLuint)state->texture);gl.ActiveTexture((GLenum)state->active);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)state->read_fbo);gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)state->draw_fbo);
    gl.Viewport(state->viewport[0],state->viewport[1],state->viewport[2],state->viewport[3]);
    gl.Scissor(state->scissor[0],state->scissor[1],state->scissor[2],state->scissor[3]);
    gl.ColorMask(state->mask[0],state->mask[1],state->mask[2],state->mask[3]);
    for(unsigned i=0;i<SWITCHES;i++){if(state->enabled[i])gl.Enable(switches[i]);else gl.Disable(switches[i]);}
}
static GLuint shader(GLenum type,const char *source) {
    GLuint shader=gl.CreateShader(type);gl.ShaderSource(shader,1,&source,NULL);gl.CompileShader(shader);
    GLint okay=0;gl.GetShaderiv(shader,GL_COMPILE_STATUS,&okay);
    if(!okay){char log[2048]={0};gl.GetShaderInfoLog(shader,sizeof log,NULL,log);fprintf(stderr,"[gpu] shader: %s\n",log);gl.DeleteShader(shader);return 0;}
    return shader;
}
int cdi_gpu_init(SDL_Renderer *renderer,SDL_Texture *activation) {
    if(gpu.checked)return gpu.available;
    gpu.checked=1;gpu.renderer=renderer;
    SDL_RendererInfo info={0};SDL_GetRendererInfo(renderer,&info);
    if(!info.name || strcmp(info.name,"opengl") || !(info.flags&SDL_RENDERER_TARGETTEXTURE) || SDL_GL_BindTexture(activation,NULL,NULL))return 0;
    SDL_GL_UnbindTexture(activation);
#define LOAD(ret,name,args) gl.name=(name##Fn)SDL_GL_GetProcAddress("gl" #name);if(!gl.name)return 0;
    GL_API(LOAD)
#undef LOAD
    GLint major=0,minor=0;gl.GetIntegerv(GL_MAJOR_VERSION,&major);gl.GetIntegerv(GL_MINOR_VERSION,&minor);
    if((major<4 || (major==4 && minor<3)) &&
       !(SDL_GL_ExtensionSupported("GL_ARB_multi_draw_indirect") && SDL_GL_ExtensionSupported("GL_ARB_draw_indirect")))return 0;
    gpu.context=SDL_GL_GetCurrentContext();gl.GetIntegerv(GL_MAX_TEXTURE_SIZE,&gpu.limit);
    if(info.max_texture_width>0 && info.max_texture_width<gpu.limit)gpu.limit=info.max_texture_width;
    if(gpu.limit<768)return 0;
    GLuint vertex=shader(GL_VERTEX_SHADER,vertex_source),fragment=shader(GL_FRAGMENT_SHADER,fragment_source);
    if(!vertex || !fragment){if(vertex)gl.DeleteShader(vertex);if(fragment)gl.DeleteShader(fragment);return 0;}
    gpu.program=gl.CreateProgram();gl.AttachShader(gpu.program,vertex);gl.AttachShader(gpu.program,fragment);gl.LinkProgram(gpu.program);
    gl.DeleteShader(vertex);gl.DeleteShader(fragment);
    GLint linked=0;gl.GetProgramiv(gpu.program,GL_LINK_STATUS,&linked);
    if(!linked){char log[2048]={0};gl.GetProgramInfoLog(gpu.program,sizeof log,NULL,log);fprintf(stderr,"[gpu] link: %s\n",log);return 0;}
    GlState previous;save_state(&previous);
    gl.GenTextures(1,&gpu.source);gl.BindTexture(GL_TEXTURE_2D,gpu.source);
    gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA32UI,MCD212_VIDEO_MAX_WIDTH+1,MCD212_VIDEO_MAX_HEIGHT,0,GL_RGBA_INTEGER,GL_UNSIGNED_INT,NULL);
    gl.GenVertexArrays(1,&gpu.vao);gpu.region_location=gl.GetUniformLocation(gpu.program,"region");
    uint32_t commands[MCD212_VIDEO_MAX_HEIGHT][4]={{0}};
    gl.GenBuffers(1,&gpu.commands);gl.BindBuffer(GL_DRAW_INDIRECT_BUFFER,gpu.commands);
    gl.BufferData(GL_DRAW_INDIRECT_BUFFER,sizeof commands,commands,GL_DYNAMIC_DRAW);
    restore_state(&previous);gpu.available=gl.GetError()==GL_NO_ERROR;
    fprintf(stderr,"[gpu] owned span renderer available=%d texture_limit=%d max_tile_bytes=67108864\n",gpu.available,gpu.limit);
    return gpu.available;
}
static int target(SDL_Texture **texture,unsigned width,unsigned height,unsigned *old_w,unsigned *old_h) {
    if(*texture && width==*old_w && height==*old_h)return 1;
    SDL_Texture *fresh=SDL_CreateTexture(gpu.renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_TARGET,(int)width,(int)height);
    if(!fresh)return 0;
    SDL_SetTextureBlendMode(fresh,SDL_BLENDMODE_NONE);
    if(*texture)SDL_DestroyTexture(*texture);
    *texture=fresh;*old_w=width;*old_h=height;return 1;
}
static GLuint framebuffer(SDL_Texture *target,GLuint *owned) {
    /* SDL owns the texture; this consumer owns its attachment. A framebuffer
     * handle observed after SDL_SetRenderTarget is not an owned attachment:
     * a later target switch can reattach another equal-sized texture. */
    if(SDL_GL_BindTexture(target,NULL,NULL))return 0;
    GLint texture=0;gl.GetIntegerv(GL_TEXTURE_BINDING_2D,&texture);SDL_GL_UnbindTexture(target);
    if(!texture)return 0;
    if(!*owned)gl.GenFramebuffers(1,owned);
    gl.BindFramebuffer(GL_FRAMEBUFFER,*owned);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,(GLuint)texture,0);
    return gl.CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE?*owned:0;
}
SDL_Texture *cdi_gpu_render(const CdiRasterFrame *frame,unsigned requested,const uint32_t *native_check) {
    if(!frame)return NULL;
    if(!gpu.available || gpu.failed || SDL_GL_GetCurrentContext()!=gpu.context) {
        if(gpu.requested!=requested)fprintf(stderr,"[gpu] portable native job fallback requested=%u effective=1 native=%ux%u\n",requested,frame->width,frame->height);
        gpu.requested=requested;gpu.effective=1;gpu.width=frame->width;gpu.height=frame->height;return NULL;
    }
    if(!requested || requested>64)return NULL;
    unsigned scale=requested;while(scale>1 && 256u*scale>(unsigned)gpu.limit)scale/=2;
    int output_w=0,output_h=0;SDL_GetRendererOutputSize(gpu.renderer,&output_w,&output_h);
    if(scale==1){output_w=frame->width;output_h=frame->height;}
    else {
        /* Resolve on one global native grid. Rounding each tile directly to
         * arbitrary window pixels changes the sampling phase at tile edges.
         * This display-sized target is independent of internal density. */
        unsigned multiple_x=output_w>0?((unsigned)output_w+frame->width-1u)/frame->width:1u;
        unsigned multiple_y=output_h>0?((unsigned)output_h+frame->height-1u)/frame->height:1u;
        if(multiple_x>(unsigned)gpu.limit/frame->width)multiple_x=(unsigned)gpu.limit/frame->width;
        if(multiple_y>(unsigned)gpu.limit/frame->height)multiple_y=(unsigned)gpu.limit/frame->height;
        output_w=(int)(multiple_x*frame->width);output_h=(int)(multiple_y*frame->height);
    }
    if(output_w<1 || output_h<1 || output_w>gpu.limit || output_h>gpu.limit)return NULL;
    if(!target(&gpu.output,(unsigned)output_w,(unsigned)output_h,&gpu.output_w,&gpu.output_h) ||
       !target(&gpu.tile,256u*scale,16u*scale,&gpu.tile_w,&gpu.tile_h))return NULL;
    if(native_check && !target(&gpu.check,frame->width,frame->height,&gpu.check_w,&gpu.check_h))return NULL;
    if(gpu.requested!=requested || gpu.effective!=scale || gpu.width!=frame->width || gpu.height!=frame->height) {
        fprintf(stderr,"[gpu] density requested=%u effective=%u virtual=%ux%u output=%ux%u tile=%ux%u\n",requested,scale,frame->width*scale,frame->height*scale,gpu.output_w,gpu.output_h,gpu.tile_w,gpu.tile_h);
    }
    gpu.requested=requested;gpu.effective=scale;gpu.width=frame->width;gpu.height=frame->height;
    if(SDL_RenderFlush(gpu.renderer))return NULL;
    SDL_Texture *old_target=SDL_GetRenderTarget(gpu.renderer);
    GlState previous;save_state(&previous);
    GLuint output=framebuffer(gpu.output,&gpu.output_fbo),check=native_check?framebuffer(gpu.check,&gpu.check_fbo):0,tile=framebuffer(gpu.tile,&gpu.tile_fbo);
    if(!output || !tile || (native_check && !check)){SDL_SetRenderTarget(gpu.renderer,old_target);restore_state(&previous);return NULL;}
    if(gpu.epoch!=frame->epoch){memset(gpu.rows,0,sizeof gpu.rows);gpu.epoch=frame->epoch;gpu.resets++;}
    cdi_render_profile_begin(0);gl.ActiveTexture(GL_TEXTURE0);gl.BindTexture(GL_TEXTURE_2D,gpu.source);
    gl.BindBuffer(GL_DRAW_INDIRECT_BUFFER,gpu.commands);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);gl.PixelStorei(GL_UNPACK_ALIGNMENT,1);
    for(unsigned i=1;i<STORES;i++)gl.PixelStorei(stores[i],0);
    CdiRasterSpan upload[MCD212_VIDEO_MAX_WIDTH+1];
    for(unsigned y=0;y<frame->height;y++) {
        const CdiRasterRow *row=frame->rows[y];if(!row || row->count>MCD212_VIDEO_MAX_WIDTH){gpu.failed=1;break;}
        if(gpu.rows[y]!=row->identity) {
            upload[0]=(CdiRasterSpan){row->count,0,0,0};memcpy(upload+1,row->spans,row->count*sizeof *row->spans);
            gl.TexSubImage2D(GL_TEXTURE_2D,0,0,(GLint)y,(GLsizei)row->count+1,1,GL_RGBA_INTEGER,GL_UNSIGNED_INT,upload);
            /* The immutable row owns its real visible-span count. Resident
             * indirect draws omit padded/hidden instances without a CPU walk
             * per enhanced tile. firstVertex carries the authored row. */
            uint32_t command[4]={4,row->count,y*4u,0};
            gl.BufferSubData(GL_DRAW_INDIRECT_BUFFER,(GLintptr)(y*sizeof command),sizeof command,command);
            uint64_t bytes=(uint64_t)(row->count+1)*sizeof *row->spans+sizeof command;gpu.upload_bytes+=bytes;gpu.uploads++;cdi_render_profile_upload(bytes);
            gpu.rows[y]=row->identity;
        }
    }
    cdi_render_profile_end(0);
    cdi_render_profile_begin(1);gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,output);
    gl.UseProgram(gpu.program);gl.Uniform1i(gl.GetUniformLocation(gpu.program,"source_jobs"),0);gl.BindVertexArray(gpu.vao);
    /* Fixed native tile geometry gives identical CPU submission work at every
     * density. No destination-pixel loops, expanded snapshots or frame upload. */
    for(unsigned y=0;y<frame->height && !gpu.failed;y+=16)for(unsigned x=0;x<frame->width;x+=256) {
        unsigned width=frame->width-x;if(width>256)width=256;
        unsigned height=frame->height-y;if(height>16)height=16;
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,tile);gl.Viewport(0,0,(GLsizei)(width*scale),(GLsizei)(height*scale));
        gl.Uniform4i(gpu.region_location,(GLint)x,(GLint)y,(GLint)width,(GLint)height);
        gl.MultiDrawArraysIndirect(GL_TRIANGLE_STRIP,(const void *)(uintptr_t)(y*16u),(GLsizei)height,16);gpu.draws++;
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER,tile);gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,output);
        gl.BlitFramebuffer(0,0,(GLint)(width*scale),(GLint)(height*scale),
            (GLint)((uint64_t)x*gpu.output_w/frame->width),(GLint)((uint64_t)y*gpu.output_h/frame->height),
            (GLint)((uint64_t)(x+width)*gpu.output_w/frame->width),(GLint)((uint64_t)(y+height)*gpu.output_h/frame->height),GL_COLOR_BUFFER_BIT,GL_NEAREST);
        if(native_check) {
            gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,check);
            gl.BlitFramebuffer(0,0,(GLint)(width*scale),(GLint)(height*scale),(GLint)x,(GLint)y,(GLint)(x+width),(GLint)(y+height),GL_COLOR_BUFFER_BIT,GL_NEAREST);
        }
    }
    cdi_render_profile_end(1);
    GLenum error=gl.GetError();
    if(error!=GL_NO_ERROR){fprintf(stderr,"[gpu] raster error0x%x; portable native fallback\n",(unsigned)error);gpu.failed=1;}
    if(native_check && !gpu.failed) {
        SDL_SetRenderTarget(gpu.renderer,gpu.check);
        cdi_render_profile_readback(gpu.renderer,native_check,frame->width,frame->height,frame->generation);
    }
    SDL_SetRenderTarget(gpu.renderer,old_target);restore_state(&previous);
    if(gpu.failed)return NULL;
    gpu.frames++;return gpu.output;
}
void cdi_gpu_state(char *out,int capacity) {
    uint64_t tile=(uint64_t)gpu.tile_w*gpu.tile_h*4u,output=(uint64_t)gpu.output_w*gpu.output_h*4u;
    snprintf(out,capacity,"{\"available\":%d,\"failed\":%d,\"frames\":%llu,\"source_uploads\":%llu,\"source_upload_bytes\":%llu,\"draw_calls\":%llu,\"resets\":%llu,\"requested_scale\":%u,\"effective_scale\":%u,\"virtual_width\":%u,\"virtual_height\":%u,\"output_width\":%u,\"output_height\":%u,\"tile_bytes\":%llu,\"owned_gpu_bytes\":%llu}",gpu.available,gpu.failed,(unsigned long long)gpu.frames,(unsigned long long)gpu.uploads,(unsigned long long)gpu.upload_bytes,(unsigned long long)gpu.draws,(unsigned long long)gpu.resets,gpu.requested,gpu.effective,gpu.width*gpu.effective,gpu.height*gpu.effective,gpu.output_w,gpu.output_h,(unsigned long long)tile,(unsigned long long)(tile+output+(gpu.commands?MCD212_VIDEO_MAX_HEIGHT*16u:0)+(gpu.source?(MCD212_VIDEO_MAX_WIDTH+1u)*MCD212_VIDEO_MAX_HEIGHT*16u:0)+(gpu.check?(uint64_t)gpu.check_w*gpu.check_h*4u:0)));
}
void cdi_gpu_reset(int lost) {
    if(!lost && gl.DeleteTextures && SDL_GL_GetCurrentContext()==gpu.context) {
        gl.DeleteTextures(1,&gpu.source);gl.DeleteVertexArrays(1,&gpu.vao);gl.DeleteProgram(gpu.program);
        gl.DeleteBuffers(1,&gpu.commands);
        gl.DeleteFramebuffers(1,&gpu.output_fbo);gl.DeleteFramebuffers(1,&gpu.check_fbo);gl.DeleteFramebuffers(1,&gpu.tile_fbo);
    }
    if(gpu.tile)SDL_DestroyTexture(gpu.tile);
    if(gpu.output)SDL_DestroyTexture(gpu.output);
    if(gpu.check)SDL_DestroyTexture(gpu.check);
    memset(&gpu,0,sizeof gpu);memset(&gl,0,sizeof gl);
}
#else
int cdi_gpu_init(SDL_Renderer *renderer,SDL_Texture *activation){(void)renderer;(void)activation;return 0;}
SDL_Texture *cdi_gpu_render(const CdiRasterFrame *frame,unsigned scale,const uint32_t *check){(void)frame;(void)scale;(void)check;return NULL;}
void cdi_gpu_state(char *out,int capacity){snprintf(out,capacity,"{\"available\":0,\"frames\":0,\"effective_scale\":1,\"tile_bytes\":0}");}
void cdi_gpu_reset(int lost){(void)lost;}
#endif
