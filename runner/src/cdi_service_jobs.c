/* Complete firmware process-ID lookup used by notification/queue services.
 * Fold the indexed lookup and its register/condition-code result as one job;
 * preserve actual RAM read times and every physical-clock/IRQ frontier. CPU
 * detail is reconstructed only for publication, interruption or observation.
 * Process tables, error E0/carry and all notification callbacks remain guest
 * owned. No persistent shadow process registry is introduced. */
#include "cdi_service_jobs.h"
#include "cdi_sha256.h"
#include "debug_server.h"
#include <string.h>

#ifndef CDI_SERVICE_HLE
#define CDI_SERVICE_HLE 0
#endif
RecompFuncPtr cdi_service_entry;
static CdiServiceJobState stats;

#if CDI_SERVICE_HLE
extern uint8_t g_rom[CDI_ROM_SIZE];
extern RecompFuncPtr recomp_native_find_entry(uint32_t address);
typedef struct {
    M68KState before;
    uint32_t table, first_descriptor, checked_descriptor, done, pc;
    uint16_t count, descriptor_id;
    int active;
} ProcessLookup;
static ProcessLookup flight;
static const uint16_t operation_pc[]={
    0x829A,0x829E,0x82A0,0x82A2,0x82A4,0x82A6,0x82A8,0x82AA,
    0x82AC,0x82AE,0x82B0,0x82B2,0x82B4,0x82B6,0x82BA,0x82BE
};
static const uint8_t operation_cycles[]={22,11,13,19,13,7,19,13,15,15,13,11,13,11,14,15};
static int ram(uint32_t address,uint32_t size) {
    return (address<CDI_RAM0_SIZE && size<=CDI_RAM0_SIZE-address) ||
        (address>=CDI_RAM1_BASE && address-CDI_RAM1_BASE<CDI_RAM1_SIZE &&
         size<=CDI_RAM1_SIZE-(address-CDI_RAM1_BASE));
}
static void logical(M68KState *s,uint32_t value,unsigned bits) {
    uint32_t mask=bits==32?UINT32_MAX:(1u<<bits)-1;
    s->SR&=(uint16_t)~0xFu;
    if(!(value&mask))s->SR|=SR_Z;
    if(value&(1u<<(bits-1)))s->SR|=SR_N;
}
static void compare(M68KState *s,uint16_t left,uint16_t right) {
    uint16_t result=(uint16_t)(left-right);
    logical(s,result,16);
    if(right>left)s->SR|=SR_C;
    if(((left^right)&(left^result))&0x8000u)s->SR|=SR_V;
}
static M68KState prefix(void) {
    M68KState s=flight.before;
    uint16_t id=(uint16_t)s.D[0],shifted=(uint16_t)(id<<2);
    uint32_t done=flight.done;
    if(done&1u)s.A[0]=flight.table;
    if(done&(1u<<1))compare(&s,id,flight.count);
    if(done&(1u<<3)) {
        s.D[0]=(s.D[0]&0xFFFF0000u)|shifted;
        s.SR&=(uint16_t)~SR_X;logical(&s,shifted,16);
        if(id&0x4000u)s.SR|=SR_C|SR_X;
        if((id^shifted)&0x8000u)s.SR|=SR_V;
    }
    if(done&(1u<<5))s.A[0]=flight.table+(uint32_t)(int32_t)(int16_t)shifted;
    if(done&(1u<<6)) {
        s.D[0]=(s.D[0]&0xFFFF0000u)|(shifted>>2);
        s.SR&=(uint16_t)~SR_X;logical(&s,shifted>>2,16);
        if(shifted&2u)s.SR|=SR_C|SR_X;
    }
    if(done&(1u<<8))s.A[1]=flight.first_descriptor;
    if(done&(1u<<9))logical(&s,flight.checked_descriptor,32);
    if(done&(1u<<11))compare(&s,shifted>>2,flight.descriptor_id);
    if(done&(1u<<13)) {
        s.D[1]=(s.D[1]&0xFFFF0000u)|0xE0u;logical(&s,0xE0,16);
    }
    if(done&(1u<<14))s.SR|=SR_C;
    s.PC=flight.pc;return s;
}
static int fallback(void) {
    g_cpu=prefix();flight.active=0;stats.fallbacks++;
    recomp_tail_call(g_cpu.PC);return 0;
}
static int begin(unsigned operation,unsigned *observation_epoch) {
    flight.pc=CDI_ROM_BASE+operation_pc[operation];g_cpu.PC=flight.pc;
    unsigned observed=debug_native_job_epoch();
    if(observed!=*observation_epoch) {
        *observation_epoch=observed;
        if(!debug_native_job_allowed(0x40829A,0x4082C0))return fallback();
    }
    if(g_recomp_irq_armed && recomp_pending_irq_mask()) {
        int level=recomp_pending_irq_level();
        if(level==7 || level>((g_cpu.SR>>8)&7)) {
            g_cpu=prefix();flight.active=0;stats.interrupted++;recomp_take_irq();
        }
    }
    return 1;
}
static void complete(unsigned operation) {
    flight.done|=1u<<operation;
    g_native_insn_count++;g_cycle_accumulator+=operation_cycles[operation];
    g_audio_cycle_counter+=operation_cycles[operation];
    if(g_cycle_accumulator>=g_vblank_threshold)glue_check_vblank();
}
static int readable(uint32_t address,uint32_t size) {
    return (!(address&1u) && ram(address,size)) || fallback();
}
static int ordinary_call(void) {
    uint32_t caller=g_cpu.PC,next=caller+2,target;
    if(!ram(caller,6) && !(caller>=CDI_ROM_BASE && caller<=CDI_MCD212_BASE-6))return 0;
    uint16_t opcode=m68k_read16(caller);
    if((opcode&0xFF00u)==0x6100u) {
        int32_t displacement=(int8_t)opcode;
        if(!(opcode&255u)){displacement=(int16_t)m68k_read16(next);next+=2;}
        target=caller+2u+(uint32_t)displacement;
    } else if(opcode==0x4EB9u){target=m68k_read32(next);next+=4;}
    else if((opcode&0xFFF8u)==0x4E90u)target=g_cpu.A[opcode&7u];
    else return 0;
    return target==0x40829Au && m68k_read32(g_cpu.A[7])==next;
}
int cdi_service_dispatch(RecompFuncPtr function) {
    unsigned observation_epoch=debug_native_job_epoch();
    if(!cdi_service_entry || function!=cdi_service_entry ||
       (g_cpu.A[7]&1u) || !ram(g_cpu.A[7],4) ||
       !debug_native_job_allowed(0x40829A,0x4082C0) || !ordinary_call())return 0;
    stats.jobs++;g_cpu.PC=0x40829A;debug_trace_block();
    flight=(ProcessLookup){.before=g_cpu,.pc=0x40829A,.active=1};
    uint16_t id=(uint16_t)g_cpu.D[0],shifted=(uint16_t)(id<<2);
    uint32_t table_address=g_cpu.A[6]+68,slot=0;
    for(unsigned operation=0;operation<16;operation++) {
        if(!begin(operation,&observation_epoch))return 1;
        switch(operation) {
        case 0:
            if(!readable(table_address,4))return 1;
            flight.table=m68k_read32(table_address);break;
        case 1:
            if(!readable(flight.table,2))return 1;
            flight.count=m68k_read16(flight.table);break;
        case 5:slot=flight.table+(uint32_t)(int32_t)(int16_t)shifted;break;
        case 8:case 9:
            if(!readable(slot,4))return 1;
            if(operation==8)flight.first_descriptor=m68k_read32(slot);
            else flight.checked_descriptor=m68k_read32(slot);
            break;
        case 11:
            if(!readable(flight.first_descriptor,2))return 1;
            flight.descriptor_id=m68k_read16(flight.first_descriptor);break;
        case 13:stats.errors++;break;
        default:break;
        }
        complete(operation);
        if((operation==2 && id>flight.count) ||
           (operation==4 && !shifted) ||
           (operation==7 && !(shifted>>2)) ||
           (operation==10 && !flight.checked_descriptor))operation=12;
        else if(operation==12 && (shifted>>2)==flight.descriptor_id)operation=14;
    }
    g_cpu=prefix();flight.active=0;return 1;
}
void cdi_service_sync_cpu(void){if(flight.active)g_cpu=prefix();}
void cdi_service_reset(void) {
    /* SHA256 of exactly the selected 490a routine, through its RTS. */
    static const uint8_t expected[32]={
        0xDE,0x70,0x3C,0xAE,0xA3,0x85,0x74,0x9C,0xEF,0xDF,0x64,0x71,0x65,0x67,0x6C,0x8A,
        0x04,0x07,0x48,0x03,0x4A,0x6B,0xE3,0xBE,0x43,0x68,0x9A,0x38,0x34,0x6E,0x5B,0x18};
    uint8_t digest[32];memset(&stats,0,sizeof stats);memset(&flight,0,sizeof flight);
    cdi_service_entry=recomp_native_find_entry(0x40829A);
    cdi_sha256(g_rom+0x829A,0x26,digest);
    if(memcmp(digest,expected,sizeof expected))cdi_service_entry=NULL;
}
#else
int cdi_service_dispatch(RecompFuncPtr function){(void)function;return 0;}
void cdi_service_sync_cpu(void){}
void cdi_service_reset(void){memset(&stats,0,sizeof stats);cdi_service_entry=NULL;}
#endif
void cdi_service_job_state(CdiServiceJobState *out){*out=stats;}
