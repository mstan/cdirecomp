/* Complete firmware rectangle fill: original guest memory and CPU remain
 * authoritative at every bus/device frontier. Native row orchestration removes
 * split-function dispatch and instruction-history copying, not guest writes,
 * ownership/invalidation notifications, clocks, interrupts or error behavior. */
#include "cdi_memory_jobs.h"
#include "debug_server.h"
#include <string.h>

#ifndef CDI_MEMORY_HLE
#define CDI_MEMORY_HLE 0
#endif
RecompFuncPtr cdi_memory_entry;
static CdiMemoryJobState stats;

#if CDI_MEMORY_HLE
extern uint8_t g_rom[CDI_ROM_SIZE];
extern RecompFuncPtr recomp_native_find_entry(uint32_t address);
static int ram(uint32_t address,uint32_t size) {
    return (address<CDI_RAM0_SIZE && size<=CDI_RAM0_SIZE-address) ||
        (address>=CDI_RAM1_BASE && address-CDI_RAM1_BASE<CDI_RAM1_SIZE &&
         size<=CDI_RAM1_SIZE-(address-CDI_RAM1_BASE));
}
static void logic(uint32_t value,unsigned bits) {
    uint32_t mask=bits==32?UINT32_MAX:(1u<<bits)-1;
    g_cpu.SR&=(uint16_t)~0xFu;
    if(!(value&mask))g_cpu.SR|=SR_Z;
    if(value&(1u<<(bits-1)))g_cpu.SR|=SR_N;
}
static uint32_t subtract(uint32_t left,uint32_t right,unsigned bits) {
    uint32_t mask=bits==32?UINT32_MAX:(1u<<bits)-1;
    left&=mask;right&=mask;uint32_t result=(left-right)&mask;
    g_cpu.SR&=(uint16_t)~SR_X;logic(result,bits);
    if(right>left)g_cpu.SR|=SR_X|SR_C;
    if(((left^right)&(left^result))&(1u<<(bits-1)))g_cpu.SR|=SR_V;
    return result;
}
static void account(unsigned cycles) {
    g_native_insn_count++;g_cycle_accumulator+=cycles;g_audio_cycle_counter+=cycles;
    if(g_cycle_accumulator>=g_vblank_threshold)glue_check_vblank();
}
static int fallback(void) {
    stats.fallbacks++;recomp_tail_call(g_cpu.PC);return 0;
}
/* All registers already represent the exact operation-entry CPU. No shadow
 * state or prefix reconstruction is necessary, including asynchronous holds. */
static int frontier(uint32_t pc,unsigned *observation_epoch) {
    g_cpu.PC=pc;
    unsigned observed=debug_native_job_epoch();
    if(observed!=*observation_epoch) {
        *observation_epoch=observed;
        if(!debug_native_job_allowed(0x4461EC,0x44621C))return fallback();
    }
    if(g_recomp_irq_armed && recomp_pending_irq_mask()) {
        int level=recomp_pending_irq_level();
        if(level==7 || level>((g_cpu.SR>>8)&7)) {
            stats.interrupted++;recomp_take_irq();
        }
    }
    return 1;
}
/* Only ordinary calls reset the generated BIOS's private split-RTS marker.
 * Exception/interior resumes and unsupported call encodings stay faithful. */
static int ordinary_call(void) {
    uint32_t caller=g_cpu.PC,next=caller+2,target;
    if(!ram(caller,6) && !(caller>=CDI_ROM_BASE && caller<=CDI_MCD212_BASE-6))return 0;
    uint16_t opcode=m68k_read16(caller);
    if((opcode&0xFF00u)==0x6100u) {
        int32_t displacement=(int8_t)opcode;
        if(!(opcode&255u)){displacement=(int16_t)m68k_read16(next);next+=2;}
        target=caller+2u+(uint32_t)displacement;
    } else if(opcode==0x4EB9u) {target=m68k_read32(next);next+=4;}
    else if((opcode&0xFFF8u)==0x4E90u)target=g_cpu.A[opcode&7u];
    else return 0;
    return target==0x4461ECu && m68k_read32(g_cpu.A[7])==next;
}
int cdi_memory_dispatch(RecompFuncPtr function) {
    unsigned observation_epoch=debug_native_job_epoch();
    uint32_t sp=g_cpu.A[7];
    if(!cdi_memory_entry || function!=cdi_memory_entry || (sp&1u) ||
       !ram(sp-28,44) || !debug_native_job_allowed(0x4461EC,0x44621C) ||
       !ordinary_call())return 0;
    stats.jobs++;
    g_cpu.PC=0x4461EC;debug_trace_block();
    /* MOVEM stores in the firmware's descending order. Stack aliases remain
     * real RAM, and the final restore re-reads any overwritten saved values. */
    m68k_write32(sp-4,g_cpu.A[0]);
    for(unsigned reg=6;reg;reg--)m68k_write32(sp-4*(8-reg),g_cpu.D[reg]);
    g_cpu.A[7]=sp-28;account(103);
    if(!frontier(0x4461F0,&observation_epoch))return 1;
    g_cpu.A[0]=g_cpu.D[0];account(7);
    if(!frontier(0x4461F2,&observation_epoch))return 1;
    g_cpu.D[2]=m68k_read32(sp+4);logic(g_cpu.D[2],32);account(22);
    if(!frontier(0x4461F6,&observation_epoch))return 1;
    g_cpu.D[4]=m68k_read32(sp+12);logic(g_cpu.D[4],32);account(22);
    if(!frontier(0x4461FA,&observation_epoch))return 1;
    g_cpu.D[4]=subtract(g_cpu.D[4],g_cpu.D[1],32);account(7);
    if(!frontier(0x4461FC,&observation_epoch))return 1;
    g_cpu.D[3]=m68k_read32(sp+8);logic(g_cpu.D[3],32);account(22);
    if(!frontier(0x446200,&observation_epoch))return 1;
    g_cpu.D[2]=(g_cpu.D[2]&0xFFFF0000u)|subtract(g_cpu.D[2],1,16);account(7);
    if(!frontier(0x446202,&observation_epoch))return 1;
    g_cpu.D[1]=(g_cpu.D[1]&0xFFFF0000u)|subtract(g_cpu.D[1],1,16);account(7);
    if(!frontier(0x446204,&observation_epoch))return 1;
    g_cpu.D[5]=g_cpu.D[1];logic(g_cpu.D[5],32);account(7);
    for(;;) {
        do {
            if(!frontier(0x446206,&observation_epoch))return 1;
            if(!ram(g_cpu.A[0],1)){fallback();return 1;}
            logic(g_cpu.D[3],8);
            m68k_write8(g_cpu.A[0],(uint8_t)g_cpu.D[3]);g_cpu.A[0]++;
            stats.bytes++;account(11);
            if(!frontier(0x446208,&observation_epoch))return 1;
            g_cpu.D[1]=(g_cpu.D[1]&0xFFFF0000u)|(uint16_t)(g_cpu.D[1]-1);
            account(17);
        }while((uint16_t)g_cpu.D[1]!=0xFFFFu);
        stats.rows++;
        if(!frontier(0x44620C,&observation_epoch))return 1;
        g_cpu.A[0]+=g_cpu.D[4];account(7);
        if(!frontier(0x44620E,&observation_epoch))return 1;
        g_cpu.D[1]=g_cpu.D[5];logic(g_cpu.D[1],32);account(7);
        if(!frontier(0x446210,&observation_epoch))return 1;
        g_cpu.D[2]=(g_cpu.D[2]&0xFFFF0000u)|(uint16_t)(g_cpu.D[2]-1);
        account(17);
        if((uint16_t)g_cpu.D[2]==0xFFFFu)break;
    }
    if(!frontier(0x446214,&observation_epoch))return 1;
    g_cpu.D[0]=g_cpu.A[0];logic(g_cpu.D[0],32);account(7);
    if(!frontier(0x446216,&observation_epoch))return 1;
    for(unsigned reg=1;reg<=6;reg++)g_cpu.D[reg]=m68k_read32(sp-28+4*(reg-1));
    g_cpu.A[0]=m68k_read32(sp-4);g_cpu.A[7]=sp;account(103);
    if(!frontier(0x44621A,&observation_epoch))return 1;
    account(15);return 1;
}
void cdi_memory_reset(void) {
    static const uint8_t expected[48]={
        0x48,0xE7,0x7E,0x80,0x20,0x40,0x24,0x2F,0x00,0x20,0x28,0x2F,0x00,0x28,0x98,0x81,
        0x26,0x2F,0x00,0x24,0x53,0x42,0x53,0x41,0x2A,0x01,0x10,0xC3,0x51,0xC9,0xFF,0xFC,
        0xD1,0xC4,0x22,0x05,0x51,0xCA,0xFF,0xF4,0x20,0x08,0x4C,0xDF,0x01,0x7E,0x4E,0x75};
    memset(&stats,0,sizeof stats);
    cdi_memory_entry=recomp_native_find_entry(0x4461EC);
    if(memcmp(g_rom+0x461EC,expected,sizeof expected))cdi_memory_entry=NULL;
}
#else
int cdi_memory_dispatch(RecompFuncPtr function){(void)function;return 0;}
void cdi_memory_reset(void){memset(&stats,0,sizeof stats);cdi_memory_entry=NULL;}
#endif
void cdi_memory_job_state(CdiMemoryJobState *out){*out=stats;}
