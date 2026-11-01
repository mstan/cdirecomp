#include "cdi_raster_jobs.h"
#include <stdlib.h>
#include <string.h>

int g_cdi_raster_capture_enabled;
#define enabled g_cdi_raster_capture_enabled
static unsigned current_row,width,height,count;
static uint64_t identity,epoch;
static CdiRasterSpan pending[MCD212_VIDEO_MAX_WIDTH],temporary[MCD212_VIDEO_MAX_WIDTH];
static CdiRasterRow *working[MCD212_VIDEO_MAX_HEIGHT];
static CdiRasterFrame *published;
static void release_row(const CdiRasterRow *row) {
    if(row && atomic_fetch_sub_explicit(&((CdiRasterRow *)row)->references,1,memory_order_acq_rel)==1)free((void *)row);
}
void cdi_raster_release(const CdiRasterFrame *frame) {
    if(!frame || atomic_fetch_sub_explicit(&((CdiRasterFrame *)frame)->references,1,memory_order_acq_rel)!=1)return;
    for(unsigned y=0;y<frame->height;y++)release_row(frame->rows[y]);
    free((void *)frame);
}
void cdi_raster_reset(void) {
    cdi_raster_release(published);published=NULL;
    for(unsigned y=0;y<MCD212_VIDEO_MAX_HEIGHT;y++){release_row(working[y]);working[y]=NULL;}
    width=height=count=0;epoch++;
}
int cdi_raster_capture(int value) {
    value=value!=0;if(value==enabled)return 0;
    cdi_raster_reset();enabled=value;return 1;
}
int cdi_raster_capturing(void){return enabled;}
uint64_t cdi_raster_epoch(void){return epoch;}
void cdi_raster_begin(unsigned row,unsigned w,unsigned h) {
    if(!enabled)return;
    if(width!=w || height!=h){cdi_raster_reset();width=w;height=h;}
    current_row=row;count=0;
}
static void append(CdiRasterSpan *spans,unsigned *n,unsigned first,unsigned last,uint32_t a,uint32_t b,uint32_t control) {
    if(last<=first || last>width || *n>=MCD212_VIDEO_MAX_WIDTH)return;
    if(*n) {
        CdiRasterSpan *previous=&spans[*n-1];
        if((previous->bounds>>16)==first && previous->a==a && previous->b==b && previous->control==control) {
            previous->bounds=(previous->bounds&65535u)|(last<<16);return;
        }
    }
    spans[(*n)++]=(CdiRasterSpan){first|(last<<16),a,b,control};
}
void cdi_raster_span(unsigned first,unsigned last,uint32_t a,uint32_t b,uint32_t control) {
    if(enabled)append(pending,&count,first,last,a,b,control);
}
void cdi_raster_overlay(unsigned first,unsigned last,uint32_t color) {
    if(!enabled || !count || first>=width)return;
    if(last>width)last=width;
    unsigned n=0;
    for(unsigned i=0;i<count;i++) {
        const CdiRasterSpan *span=&pending[i];unsigned a=span->bounds&65535u,b=span->bounds>>16;
        if(b<=first || a>=last)append(temporary,&n,a,b,span->a,span->b,span->control);
        else {
            if(a<first)append(temporary,&n,a,first,span->a,span->b,span->control);
            append(temporary,&n,a>first?a:first,b<last?b:last,color,0,0);
            if(b>last)append(temporary,&n,last,b,span->a,span->b,span->control);
        }
    }
    memcpy(pending,temporary,n*sizeof *pending);count=n;
}
void cdi_raster_finish(void) {
    if(!enabled || !count || current_row>=height || current_row>=MCD212_VIDEO_MAX_HEIGHT)return;
    CdiRasterRow *old=working[current_row];
    if(old && old->count==count && !memcmp(old->spans,pending,count*sizeof *pending))return;
    CdiRasterRow *row=malloc(sizeof *row+count*sizeof *pending);
    if(!row){cdi_raster_capture(0);return;} /* native pixels stay authoritative */
    atomic_init(&row->references,1);row->count=count;row->identity=++identity;
    memcpy(row->spans,pending,count*sizeof *pending);working[current_row]=row;release_row(old);
}
void cdi_raster_publish(const uint32_t *native,unsigned w,unsigned h,uint64_t generation) {
    if(!enabled || !w || !h || w>MCD212_VIDEO_MAX_WIDTH || h>MCD212_VIDEO_MAX_HEIGHT)return;
    if(width!=w || height!=h){cdi_raster_reset();width=w;height=h;}
    /* Missing/retained rows after reset or geometry changes have no historical
     * source job. Capture their native authored pixels once, never at S size. */
    for(unsigned y=0;y<h;y++)if(!working[y]) {
        current_row=y;count=0;
        for(unsigned x=0;x<w;) {
            unsigned end=x+1;uint32_t color=native[(size_t)y*w+x];
            while(end<w && native[(size_t)y*w+end]==color)end++;
            append(pending,&count,x,end,color,0,0);x=end;
        }
        cdi_raster_finish();if(!enabled)return;
    }
    CdiRasterFrame *frame=calloc(1,sizeof *frame);
    if(!frame){cdi_raster_capture(0);return;}
    atomic_init(&frame->references,1);frame->width=(uint16_t)w;frame->height=(uint16_t)h;
    frame->generation=generation;frame->epoch=epoch;
    for(unsigned y=0;y<h;y++) {
        frame->rows[y]=working[y];atomic_fetch_add_explicit(&working[y]->references,1,memory_order_relaxed);
    }
    CdiRasterFrame *old=published;published=frame;cdi_raster_release(old);
}
const CdiRasterFrame *cdi_raster_acquire(void) {
    if(published)atomic_fetch_add_explicit(&published->references,1,memory_order_relaxed);
    return published;
}
static unsigned channel(unsigned color,unsigned factor) {
    int value=(int)factor*((int)color-16)/63+16;
    return value<0?0u:value>255?255u:(unsigned)value;
}
uint32_t cdi_raster_color(const CdiRasterSpan *span) {
    unsigned mode=span->control&3u;if(!mode)return span->a;
    unsigned fa=(span->control>>2)&63u,fb=(span->control>>8)&63u;
    uint32_t result=0xff000000u;
    for(unsigned shift=0;shift<24;shift+=8) {
        int value=(int)channel((span->a>>shift)&255u,fa);
        if(mode==2)value+=(int)channel((span->b>>shift)&255u,fb)-16;
        result|=(uint32_t)(value<0?0:value>255?255:value)<<shift;
    }
    return result;
}
int cdi_raster_native(const CdiRasterFrame *frame,uint32_t *pixels,uint32_t capacity) {
    if(!frame || !pixels || (uint32_t)frame->width*frame->height>capacity)return 0;
    for(unsigned y=0;y<frame->height;y++) {
        const CdiRasterRow *row=frame->rows[y];if(!row)return 0;
        for(unsigned i=0;i<row->count;i++) {
            const CdiRasterSpan *span=&row->spans[i];uint32_t color=cdi_raster_color(span);
            for(unsigned x=span->bounds&65535u;x<(span->bounds>>16);x++)pixels[(size_t)y*frame->width+x]=color;
        }
    }
    return 1;
}
