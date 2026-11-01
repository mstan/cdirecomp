#include "cdi_render_profile.h"
#include "mcd212_video.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {PASSES=3,SLOTS=16};
/* ARB_timer_query: timestamps complete after prior GL commands, results polled
 * with QUERY_RESULT_AVAILABLE. No CPU elapsed timer is used as GPU time.
 * https://registry.khronos.org/OpenGL/extensions/ARB/ARB_timer_query.txt */
typedef void (APIENTRY *GenQueriesFn)(GLsizei,GLuint *);
typedef void (APIENTRY *DeleteQueriesFn)(GLsizei,const GLuint *);
typedef void (APIENTRY *QueryCounterFn)(GLuint,GLenum);
typedef void (APIENTRY *GetQueryObjectivFn)(GLuint,GLenum,GLint *);
typedef void (APIENTRY *GetQueryObjectui64vFn)(GLuint,GLenum,uint64_t *);
typedef void (APIENTRY *GetQueryivFn)(GLenum,GLenum,GLint *);
typedef const GLubyte *(APIENTRY *GetStringFn)(GLenum);
typedef void (APIENTRY *GetIntegervFn)(GLenum,GLint *);
static struct {
    int checked,enabled,gpu,issuing[PASSES],nvx;
    GenQueriesFn gen;DeleteQueriesFn destroy;QueryCounterFn counter;
    GetQueryObjectivFn available;GetQueryObjectui64vFn result;GetQueryivFn query;
    GetStringFn string;GetIntegervFn integer;
    GLuint queries[SLOTS][PASSES][2];uint8_t pending[SLOTS][PASSES];
    uint64_t frames,uploads,bytes,cpu_start,cpu_ns,reads,mismatches,cpu_hash,gpu_hash,read_generation;
    uint64_t count[PASSES],ns[PASSES],max_ns[PASSES],missed;
    unsigned read_stride;
    GLint gpu_total_kib,gpu_free_kib;
    char renderer[128],vendor[128],version[128];
    uint32_t *pixels;
} profile;
static uint64_t hash(const uint32_t *pixels,size_t bytes) {
    uint64_t value=UINT64_C(0x14650fb0739d0383);const uint8_t *p=(const uint8_t *)pixels;
    while(bytes--){value^=*p++;value*=UINT64_C(0x100000001b3);}return value;
}
int cdi_render_profile_init(SDL_Renderer *renderer,SDL_Texture *activation) {
    if(profile.checked)return profile.enabled;
    profile.checked=1;
    if(!getenv("CDI_GPU_PROFILE"))return 0;
    profile.enabled=1;profile.read_stride=60;
    if(getenv("CDI_GPU_READBACK_STRIDE"))profile.read_stride=(unsigned)strtoul(getenv("CDI_GPU_READBACK_STRIDE"),NULL,10);
    SDL_RendererInfo info={0};SDL_GetRendererInfo(renderer,&info);
    snprintf(profile.renderer,sizeof profile.renderer,"%s",info.name?info.name:"unknown");
    if(!info.name || strcmp(info.name,"opengl") || SDL_GL_BindTexture(activation,NULL,NULL))return 1;
    SDL_GL_UnbindTexture(activation);
#define LOAD(field,type,name) do{profile.field=(type)SDL_GL_GetProcAddress(name);if(!profile.field)return 1;}while(0)
    LOAD(gen,GenQueriesFn,"glGenQueries");LOAD(destroy,DeleteQueriesFn,"glDeleteQueries");
    LOAD(counter,QueryCounterFn,"glQueryCounter");LOAD(available,GetQueryObjectivFn,"glGetQueryObjectiv");
    LOAD(result,GetQueryObjectui64vFn,"glGetQueryObjectui64v");LOAD(query,GetQueryivFn,"glGetQueryiv");
    LOAD(string,GetStringFn,"glGetString");LOAD(integer,GetIntegervFn,"glGetIntegerv");
#undef LOAD
    GLint bits=0;profile.query(GL_TIMESTAMP,GL_QUERY_COUNTER_BITS,&bits);if(bits<30)return 1;
    profile.gen(SLOTS*PASSES*2,&profile.queries[0][0][0]);profile.gpu=1;
    snprintf(profile.renderer,sizeof profile.renderer,"%s",profile.string(GL_RENDERER));
    snprintf(profile.vendor,sizeof profile.vendor,"%s",profile.string(GL_VENDOR));
    snprintf(profile.version,sizeof profile.version,"%s",profile.string(GL_VERSION));
    const char *extensions=(const char *)profile.string(GL_EXTENSIONS);
    profile.nvx=extensions && strstr(extensions,"GL_NVX_gpu_memory_info");
    if(profile.nvx){profile.integer(0x9047,&profile.gpu_total_kib);profile.integer(0x9049,&profile.gpu_free_kib);}
    fprintf(stderr,"[render] hardware profile vendor=%s renderer=%s GL=%s timestamp_bits=%d\n",profile.vendor,profile.renderer,profile.version,bits);
    return 1;
}
int cdi_render_profile_enabled(void){return profile.enabled;}
void cdi_render_profile_frame(void) {
    if(!profile.enabled)return;
    profile.frames++;profile.cpu_start=SDL_GetPerformanceCounter();
}
void cdi_render_profile_begin(unsigned pass) {
    if(!profile.gpu || pass>=PASSES)return;
    unsigned slot=(unsigned)(profile.frames%SLOTS);profile.issuing[pass]=0;
    if(profile.pending[slot][pass]) {
        GLint ready=0;profile.available(profile.queries[slot][pass][1],GL_QUERY_RESULT_AVAILABLE,&ready);
        if(!ready){profile.missed++;return;}
        uint64_t first,last;profile.result(profile.queries[slot][pass][0],GL_QUERY_RESULT,&first);
        profile.result(profile.queries[slot][pass][1],GL_QUERY_RESULT,&last);
        uint64_t duration=last-first;profile.count[pass]++;profile.ns[pass]+=duration;
        if(duration>profile.max_ns[pass])profile.max_ns[pass]=duration;
    }
    profile.counter(profile.queries[slot][pass][0],GL_TIMESTAMP);profile.issuing[pass]=1;
}
void cdi_render_profile_end(unsigned pass) {
    if(!profile.gpu || pass>=PASSES || !profile.issuing[pass])return;
    unsigned slot=(unsigned)(profile.frames%SLOTS);
    profile.counter(profile.queries[slot][pass][1],GL_TIMESTAMP);profile.pending[slot][pass]=1;
}
void cdi_render_profile_upload(uint64_t bytes){if(profile.enabled && bytes){profile.uploads++;profile.bytes+=bytes;}}
void cdi_render_profile_readback(SDL_Renderer *renderer,const uint32_t *expected,unsigned width,unsigned height,uint64_t generation) {
    if(!profile.enabled || !profile.read_stride || generation%profile.read_stride)return;
    if(!profile.pixels)profile.pixels=malloc(MCD212_VIDEO_MAX_WIDTH*MCD212_VIDEO_MAX_HEIGHT*sizeof(uint32_t));
    if(!profile.pixels || !width || !height || width>MCD212_VIDEO_MAX_WIDTH || height>MCD212_VIDEO_MAX_HEIGHT)return;
    if(SDL_RenderReadPixels(renderer,NULL,SDL_PIXELFORMAT_ARGB8888,profile.pixels,(int)width*4))return;
    profile.reads++;profile.read_generation=generation;
    profile.cpu_hash=hash(expected,(size_t)width*height*4);profile.gpu_hash=hash(profile.pixels,(size_t)width*height*4);
    if(memcmp(expected,profile.pixels,(size_t)width*height*4))profile.mismatches++;
    if(profile.gpu && profile.nvx)profile.integer(0x9049,&profile.gpu_free_kib);
}
void cdi_render_profile_finish(void) {
    if(profile.enabled)profile.cpu_ns+=(SDL_GetPerformanceCounter()-profile.cpu_start)*UINT64_C(1000000000)/SDL_GetPerformanceFrequency();
}
void cdi_render_profile_state(char *out,int capacity) {
    snprintf(out,capacity,"{\"ok\":true,\"enabled\":%d,\"gpu_timestamps\":%d,\"renderer\":\"%s\",\"vendor\":\"%s\",\"version\":\"%s\",\"frames\":%llu,\"uploads\":%llu,\"upload_bytes\":%llu,\"cpu_ns\":%llu,\"gpu_counts\":[%llu,%llu,%llu],\"gpu_ns\":[%llu,%llu,%llu],\"gpu_max_ns\":[%llu,%llu,%llu],\"query_misses\":%llu,\"native_readbacks\":%llu,\"native_mismatches\":%llu,\"readback_generation\":%llu,\"cpu_hash\":\"%016llx\",\"gpu_hash\":\"%016llx\",\"gpu_total_kib\":%d,\"gpu_free_kib\":%d}",
        profile.enabled,profile.gpu,profile.renderer,profile.vendor,profile.version,
        (unsigned long long)profile.frames,(unsigned long long)profile.uploads,(unsigned long long)profile.bytes,(unsigned long long)profile.cpu_ns,
        (unsigned long long)profile.count[0],(unsigned long long)profile.count[1],(unsigned long long)profile.count[2],
        (unsigned long long)profile.ns[0],(unsigned long long)profile.ns[1],(unsigned long long)profile.ns[2],
        (unsigned long long)profile.max_ns[0],(unsigned long long)profile.max_ns[1],(unsigned long long)profile.max_ns[2],
        (unsigned long long)profile.missed,(unsigned long long)profile.reads,(unsigned long long)profile.mismatches,
        (unsigned long long)profile.read_generation,(unsigned long long)profile.cpu_hash,(unsigned long long)profile.gpu_hash,profile.gpu_total_kib,profile.gpu_free_kib);
}
void cdi_render_profile_shutdown(void) {
    if(profile.gpu)profile.destroy(SLOTS*PASSES*2,&profile.queries[0][0][0]);
    free(profile.pixels);memset(&profile,0,sizeof profile);
}
