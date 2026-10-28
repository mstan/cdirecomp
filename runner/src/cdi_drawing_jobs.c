/* Native deferred drawing preparation. The queue and its callback-visible
 * state stay in guest RAM. Host orchestration replaces repeated CPU/trace
 * bookkeeping; callbacks still execute their real guest implementation.
 * Device time advances at the original quanta, with exact CPU materialization
 * at IRQ, invalidation, callback and debugger boundaries. */
#include "cdi_drawing_jobs.h"
#include "cdi_native.h"
#include "debug_server.h"
#include <string.h>

#ifndef CDI_DRAWING_HLE
#define CDI_DRAWING_HLE 0
#endif
RecompFuncPtr cdi_drawing_entry;
static CdiDrawingJobState stats;

#if CDI_DRAWING_HLE
static const CdiNativeModule *drawing_module;
typedef struct { uint32_t base; uint64_t token,epoch; } DrawFrame;
typedef struct {
    uint32_t d0,d4,d5,a0,a5,a6,sp;
    uint16_t sr;
} DrawRegisters;
typedef struct {
    DrawRegisters before;
    DrawFrame *frame;
    uint32_t read[4],pc;
    unsigned first,count,completed;
} DrawFlight;
static DrawFlight *active;
typedef struct { uint16_t pc; uint8_t cycles; } DrawOp;
static const DrawOp ops[]={
    {0x7018,25},{0x701C,70},{0x7020,7},{0x7022,7},{0x7024,13},{0x7026,7},{0x7028,13},
    {0x702A,22},{0x702E,22},{0x7032,13},
    {0x7034,22},{0x7038,22},{0x703C,22},{0x7040,15},{0x7042,18},
    {0x7044,30},{0x7048,22},{0x704C,22},{0x7050,13},{0x7052,37},
    {0x7058,7},{0x705A,7},{0x705C,13},{0x705E,13},
    {0x7060,37},{0x7066,37},{0x706C,63},{0x7072,15},{0x7074,15}
};
static uint32_t op_pc(const DrawFrame *f,unsigned op) {return f->base+0x10000u+ops[op].pc;}
static int ram_range(uint32_t a,uint32_t n) {
    return !(a&1u) && ((a<CDI_RAM0_SIZE && n<=CDI_RAM0_SIZE-a) ||
        (a>=CDI_RAM1_BASE && a-CDI_RAM1_BASE<CDI_RAM1_SIZE && n<=CDI_RAM1_SIZE-(a-CDI_RAM1_BASE)));
}
static void logic(M68KState *s,uint32_t value) {
    s->SR=(uint16_t)((s->SR&~0xFu)|(value?0:SR_Z)|((value>>31)?SR_N:0));
}
static void compare(M68KState *s,uint32_t a,uint32_t b) {
    uint32_t r=a-b;logic(s,r);
    if(b>a)s->SR|=SR_C;
    if(((a^b)&(a^r))>>31)s->SR|=SR_V;
}
static void add(M68KState *s,uint32_t a,uint32_t b) {
    uint32_t r=a+b;s->SR&=(uint16_t)~SR_X;logic(s,r);
    if((uint64_t)a+b>UINT32_MAX)s->SR|=SR_X|SR_C;
    if((~(a^b)&(a^r))>>31)s->SR|=SR_V;
}
/* Exceptional state only: no memory access, clocks, callbacks or side effects.
 * Production folds each completed queue segment directly below. */
static M68KState prefix(const DrawFlight *f) {
    M68KState s=g_cpu;
    s.D[0]=f->before.d0;s.D[4]=f->before.d4;s.D[5]=f->before.d5;
    s.A[0]=f->before.a0;s.A[5]=f->before.a5;s.A[6]=f->before.a6;
    s.A[7]=f->before.sp;s.SR=f->before.sr;
    for(unsigned i=0;i<f->completed;i++)switch(f->first+i) {
    case 0:s.A[7]-=4;s.A[5]=s.A[7];break;
    case 1:s.A[7]-=16;break;
    case 2:s.D[4]=s.D[0];logic(&s,s.D[4]);break;
    case 3:logic(&s,s.D[4]);break;
    case 5:s.D[5]=0;logic(&s,0);break;
    case 7:s.D[0]=f->read[0];logic(&s,s.D[0]);break;
    case 8:compare(&s,s.D[0],f->read[1]);break;
    case 10:s.A[0]=f->read[0];break;
    case 11:s.D[0]=f->read[1];logic(&s,s.D[0]);break;
    case 12:s.A[0]=f->read[2];break;
    case 13:s.A[0]=f->read[3];break;
    case 14:s.A[7]-=4;break;
    case 15:add(&s,f->read[0]-8u,8);break;
    case 16:s.D[0]=f->read[1];logic(&s,s.D[0]);break;
    case 17:compare(&s,s.D[0],f->read[2]);break;
    case 19:logic(&s,f->read[3]);break;
    case 20:add(&s,s.D[5],1);s.D[5]++;break;
    case 21:compare(&s,s.D[5],s.D[4]);break;
    case 24:logic(&s,f->read[0]);break;
    case 25:logic(&s,f->read[1]);break;
    case 26:s.D[4]=f->read[0];s.D[5]=f->read[1];s.A[0]=f->read[2];break;
    case 27:s.A[7]=s.A[5]+4;s.A[5]=f->read[3];break;
    default:break;
    }
    s.PC=f->pc;return s;
}
/* A completed segment publishes only its changed registers. Other guest
 * registers never left the authoritative CPU; full state is materialized
 * only at exceptional frontiers above, not copied around every empty poll. */
static void finish(const DrawFlight *f) {
    M68KState *s=&g_cpu;
    switch(f->first) {
    case 0:s->A[5]=f->before.sp-4;s->A[7]=f->before.sp-20;s->D[4]=f->before.d0;
        if(f->count==7){s->D[5]=0;logic(s,0);}else logic(s,s->D[4]);break;
    case 7:s->D[0]=f->read[0];compare(s,s->D[0],f->read[1]);break;
    case 10:s->D[0]=f->read[1];s->A[0]=f->read[3];s->A[7]=f->before.sp-4;logic(s,s->D[0]);break;
    case 15:s->D[0]=f->read[1];s->SR&=(uint16_t)~SR_X;
        if(f->read[0]<8)s->SR|=SR_X;
        if(f->count==5)logic(s,f->read[3]);else compare(s,s->D[0],f->read[2]);break;
    case 20:s->SR&=(uint16_t)~SR_X;if(f->before.d5==UINT32_MAX)s->SR|=SR_X;
        s->D[5]=f->before.d5+1;compare(s,s->D[5],s->D[4]);break;
    case 21:compare(s,s->D[5],s->D[4]);break;
    case 24:logic(s,f->read[1]);break;
    case 26:s->D[4]=f->read[0];s->D[5]=f->read[1];s->A[0]=f->read[2];
        s->A[7]=f->before.a5+4;s->A[5]=f->read[3];break;
    default:break;
    }
    s->PC=f->pc;
}
static int irq_ready(void) {
    if(!g_recomp_irq_armed || !recomp_pending_irq_mask())return 0;
    int level=recomp_pending_irq_level();return level==7 || level>((g_cpu.SR>>8)&7);
}
static void fallback(DrawFlight *f) {
    g_cpu=prefix(f);active=NULL;stats.fallbacks++;recomp_tail_call(g_cpu.PC);
}
/* Reads/writes occur at their actual operation frontiers. Even a callback
 * that changes A6, ring bounds, D4/D5 or its return address stays authoritative. */
static int memory_action(DrawFlight *f,unsigned op) {
    const DrawRegisters *s=&f->before;
    uint32_t *r=f->read,a6=s->a6,sp=s->sp,a;
    switch(op) {
    case 0:if(!ram_range(sp-4,4))return 0;m68k_write32(sp-4,s->a5);break;
    case 1:if(!ram_range(sp-20,16))return 0;
        m68k_write32(sp-8,s->a0);m68k_write32(sp-12,s->d5);
        m68k_write32(sp-16,s->d4);m68k_write32(sp-20,s->d0);break;
    case 7:if(!ram_range(a6-0x6E52,4))return 0;r[0]=m68k_read32(a6-0x6E52);break;
    case 8:if(!ram_range(a6-0x6E4E,4))return 0;r[1]=m68k_read32(a6-0x6E4E);break;
    case 10:if(!ram_range(a6-0x6E4E,4))return 0;r[0]=m68k_read32(a6-0x6E4E);break;
    case 11:if(!ram_range(r[0]+4,4))return 0;r[1]=m68k_read32(r[0]+4);break;
    case 12:if(!ram_range(a6-0x6E4E,4))return 0;r[2]=m68k_read32(a6-0x6E4E);break;
    case 13:if(!ram_range(r[2],4))return 0;r[3]=m68k_read32(r[2]);break;
    case 14:if(!ram_range(sp-4,4))return 0;m68k_write32(sp-4,f->frame->base+0x17044);break;
    case 15:if(!ram_range(a6-0x6E4E,4))return 0;
        r[0]=m68k_read32(a6-0x6E4E)+8;m68k_write32(a6-0x6E4E,r[0]);break;
    case 16:if(!ram_range(a6-0x6E4E,4))return 0;r[1]=m68k_read32(a6-0x6E4E);break;
    case 17:if(!ram_range(a6-0x6E56,4))return 0;r[2]=m68k_read32(a6-0x6E56);break;
    case 19:if(!ram_range(a6-0x6E5A,16))return 0;
        r[3]=m68k_read32(a6-0x6E5A);m68k_write32(a6-0x6E4E,r[3]);break;
    case 24:case 25:if(!ram_range(a6-0x6E5A,16))return 0;
        r[op-24]=m68k_read32(a6-0x6E5A);m68k_write32(a6-(op==24?0x6E52:0x6E4E),r[op-24]);break;
    case 26:a=s->a5-12;if(!ram_range(a,12))return 0;
        r[0]=m68k_read32(a);r[1]=m68k_read32(a+4);r[2]=m68k_read32(a+8);break;
    case 27:if(!ram_range(s->a5,4))return 0;r[3]=m68k_read32(s->a5);break;
    default:break;
    }
    return 1;
}
static int segment(DrawFrame *frame,unsigned first,unsigned count) {
    DrawFlight f;
    f.before=(DrawRegisters){g_cpu.D[0],g_cpu.D[4],g_cpu.D[5],g_cpu.A[0],
                            g_cpu.A[5],g_cpu.A[6],g_cpu.A[7],g_cpu.SR};
    f.frame=frame;f.first=first;f.count=count;f.completed=0;active=&f;
    for(unsigned i=0;i<count;i++) {
        unsigned op=first+i;f.completed=i;f.pc=op_pc(frame,op);g_cpu.PC=f.pc;
        if(frame->epoch!=g_cdi_native_invalidation_epoch) {
            frame->epoch=g_cdi_native_invalidation_epoch;
            if(!cdi_native_instruction_valid(f.pc,drawing_module->dispatch,frame->token)) {
                g_cpu=prefix(&f);active=NULL;g_redirect_addr=g_cpu.PC;g_redirect_pending=1;return 0;
            }
        }
        if(irq_ready()) {g_cpu=prefix(&f);active=NULL;stats.interrupted++;recomp_take_irq();}
        if(!memory_action(&f,op)){fallback(&f);return 0;}
        f.completed=i+1;
        g_native_insn_count++;g_cycle_accumulator+=ops[op].cycles;g_audio_cycle_counter+=ops[op].cycles;
        if(g_cycle_accumulator>=g_vblank_threshold)glue_check_vblank();
        if(first==15 && op==18) {
            if(f.read[1]<=f.read[2]){f.count=count=4;break;}
        }
    }
    finish(&f);active=NULL;return 1;
}
int cdi_drawing_dispatch(RecompFuncPtr function) {
    DrawFrame frame;
    if(!cdi_drawing_entry || function!=cdi_drawing_entry ||
       !cdi_native_job_binding(drawing_module,g_cpu.PC,&frame.base,&frame.token) ||
       !debug_native_job_allowed(frame.base+0x17018,frame.base+0x17076) ||
       ((int32_t)g_cpu.D[0]>1024) || !ram_range(g_cpu.A[7]-20,24) ||
       !ram_range(g_cpu.A[6]-0x6E5A,16))return 0;
    /* Direct BSR callers establish an ordinary JSR frame and clear the
     * generated module's private split-RTS marker. Other entry shapes retain
     * the reference; no generated private bookkeeping is guessed here. */
    uint32_t caller=g_cpu.PC,word=m68k_read16(caller),next=caller+2;
    if((word&0xFF00u)!=0x6100u)return 0;
    int32_t displacement=(int8_t)word;
    if(!(word&255u)){displacement=(int16_t)m68k_read16(next);next+=2;}
    if(caller+2u+(uint32_t)displacement!=frame.base+0x17018 ||
       m68k_read32(g_cpu.A[7])!=next)return 0;
    frame.epoch=g_cdi_native_invalidation_epoch;stats.jobs++;
    g_cpu.PC=frame.base+0x17018;debug_trace_block();
    if(!g_cpu.D[0]) {
        if(!segment(&frame,0,5) || !segment(&frame,24,2))return 1;
    } else {
        if(!segment(&frame,0,7) || !segment(&frame,21,2))return 1;
        while((int32_t)g_cpu.D[5]<(int32_t)g_cpu.D[4]) {
            if(!segment(&frame,7,3))return 1;
            if(!(g_cpu.SR&SR_Z)) {
                if(!segment(&frame,10,5))return 1;
                stats.callbacks++;recomp_call_addr(g_cpu.A[0]);
                if(g_halted || g_redirect_pending || g_rte_resume)return 1;
                if(g_rte_pending){g_rte_pending=0;return 1;}
                if(!g_call_was_hybrid) {
                    uint32_t ret=m68k_read32(g_cpu.A[7]);g_cpu.A[7]+=4;
                    if((ret&0xFFFFFFu)!=frame.base+0x17044) {
                        g_redirect_addr=ret&0xFFFFFFu;g_redirect_pending=1;return 1;
                    }
                }
                if(!segment(&frame,15,5))return 1;
            }else stats.empty_checks++;
            if(!segment(&frame,20,3))return 1;
        }
        if(!segment(&frame,23,1))return 1;
    }
    segment(&frame,26,3);return 1;
}
void cdi_drawing_sync_cpu(void) {if(active)g_cpu=prefix(active);}
void cdi_drawing_reset(void) {
    static const uint8_t digest[32]={
        0x55,0xef,0xd9,0x3e,0x44,0xfd,0xc3,0x1a,0xce,0xae,0xd4,0xd2,0x65,0x1d,0x64,0x8c,
        0xc3,0x23,0x70,0xa9,0x5d,0xa7,0xaf,0xab,0xfd,0x57,0x40,0x34,0x0a,0x44,0x29,0x6b};
    memset(&stats,0,sizeof stats);active=NULL;
    cdi_drawing_entry=cdi_native_find_function(digest,0x17018,&drawing_module);
}
#else
int cdi_drawing_dispatch(RecompFuncPtr function){(void)function;return 0;}
void cdi_drawing_sync_cpu(void){}
void cdi_drawing_reset(void){memset(&stats,0,sizeof stats);cdi_drawing_entry=NULL;}
#endif
void cdi_drawing_job_state(CdiDrawingJobState *out){*out=stats;}
