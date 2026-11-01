/* Actual generated process lookup versus the production-object native job. */
#include "cdi_runtime.h"
#include "cdi_service_jobs.h"
#include "cdi_native.h"
#include "cdi_sha256.h"
#include "debug_server.h"
#include "cdi_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint8_t g_ram0[CDI_RAM0_SIZE],g_ram1[CDI_RAM1_SIZE],g_rom[CDI_ROM_SIZE];
extern void cdi_bus_load_rom(const uint8_t *,uint32_t);
extern void runtime_reset_cpu_interrupt_state(void);
extern void mcd212_reset(void);
extern RecompFuncPtr recomp_native_find_entry(uint32_t);
extern uint64_t g_stop_seq,g_stop_frame;
static unsigned clock_variant;
static uint32_t table_slot,requested_id;
void __real_glue_check_vblank(void);
void __wrap_glue_check_vblank(void) {
    __real_glue_check_vblank();
    if(g_cpu.PC==0x4082AC && (clock_variant==10 || clock_variant==11)) {
        unsigned variant=clock_variant;clock_variant=0;
        if(variant==10){g_stop_seq=UINT64_MAX;atomic_fetch_add_explicit(&g_debug_native_observation_epoch,1,memory_order_release);}
        else m68k_write32(table_slot,0x31000);
    } else if(g_cpu.PC==0x4082B0 && clock_variant==12) {
        clock_variant=0;m68k_write16(0x30000,(uint16_t)(requested_id+1));
    }
}
static int load(const char *path,uint8_t *p,size_t size){FILE *f=fopen(path,"rb");if(!f)return 0;size_t n=fread(p,1,size,f);fclose(f);return n==size;}
static void hash(FILE *f,const void *p,size_t size){uint8_t digest[32];cdi_sha256(p,size,digest);fputc('"',f);for(unsigned i=0;i<32;i++)fprintf(f,"%02x",digest[i]);fputc('"',f);}
int main(int argc,char **argv) {
    if(argc!=11)return 2;
    FILE *rom=fopen(argv[1],"rb");if(!rom)return 2;
    uint8_t *raw=calloc(CDI_ROM_SIZE,1);if(!raw)return 2;
    size_t n=fread(raw,1,CDI_ROM_SIZE,rom);fclose(rom);cdi_bus_load_rom(raw,(uint32_t)n);free(raw);
    if(!load(argv[2],g_ram0,CDI_RAM0_SIZE)||!load(argv[3],g_ram1,CDI_RAM1_SIZE))return 2;
    int native=!strcmp(argv[5],"native");requested_id=(uint32_t)strtoul(argv[6],NULL,0);
    uint32_t count=(uint32_t)strtoul(argv[7],NULL,0);
    unsigned variant=(unsigned)strtoul(argv[8],NULL,0),irq_cycle=(unsigned)strtoul(argv[9],NULL,0),seed=(unsigned)strtoul(argv[10],NULL,0);
    cdi_media_init();runtime_init();runtime_reset_cpu_interrupt_state();periph_reset();nvram_reset();cdi_input_reset();mcd212_reset();
    g_total_cycles=g_frame_count=g_native_insn_count=0;g_cycle_accumulator=g_audio_cycle_counter=0;g_vblank_threshold=1;
    g_stop_seq=native?0:UINT64_MAX;g_stop_frame=0;g_hold_on_fault=0;
    for(unsigned i=0;i<8;i++){g_cpu.D[i]=0xA5320000u+i;g_cpu.A[i]=0x57A20000u+i;}
    uint32_t sp=0x70000,table=0x20000;g_cpu.A[7]=sp;g_cpu.A[6]=0x10000;g_cpu.PC=0x4096B4;
    g_cpu.D[0]=requested_id;g_cpu.SR=(uint16_t)(0x2000|(seed&31));g_cpu.SSP=sp;g_cpu.USP=0x60000;
    m68k_write32(sp,0x4096B8);m68k_write32(g_cpu.A[6]+68,table);m68k_write16(table,(uint16_t)count);
    table_slot=table+(uint32_t)(int32_t)(int16_t)(requested_id<<2);
    m68k_write32(table_slot,0x30000);m68k_write16(0x30000,(uint16_t)(requested_id&0x3fff));m68k_write16(0x31000,(uint16_t)(requested_id&0x3fff));
    if(variant==2)m68k_write32(table_slot,0);
    if(variant==3)m68k_write16(0x30000,(uint16_t)(requested_id+1));
    if(variant==4)m68k_write32(g_cpu.A[6]+68,CDI_RAM0_SIZE-1);
    if(variant==5)g_cpu.A[6]++;
    if(variant==6)m68k_write32(table_slot,0x100000);
    if(variant==7){g_rom[0x829A]^=1;cdi_service_reset();}
    if(variant==8){g_cpu.PC=0x40829E;g_cpu.A[0]=table;}
    if(variant==9)g_stop_seq=UINT64_MAX;
    if(variant>=10)clock_variant=variant;
    if(irq_cycle){unsigned ticks=(irq_cycle+95)/96,phase=ticks*96-irq_cycle;
        periph_increment_timer(phase);periph_write(0x80002022,0,2);periph_write(0x80002024,(0x10000u-ticks)&0xFFFFu,2);
        periph_write(0x80002045,5,1);m68k_write32((56u+5u)*4u,0x4004B8);
    }
    RecompFuncPtr entry=recomp_native_find_entry(variant==8?0x40829E:0x40829A);if(!entry)return 4;
    int interrupted=0,fault=0;g_recomp_bus_armed=g_recomp_irq_armed=1;
    if(setjmp(g_recomp_bus_env))fault=1;
    else if(setjmp(g_recomp_irq_env))interrupted=1;
    else recomp_call_func(entry);
    g_recomp_bus_armed=g_recomp_irq_armed=0;
    FILE *out=fopen(argv[4],"wb");if(!out)return 2;
    fprintf(out,"{\"cpu\":{\"pc\":%u,\"sr\":%u,\"usp\":%u,\"ssp\":%u,\"d\":[",g_cpu.PC,g_cpu.SR,g_cpu.USP,g_cpu.SSP);
    for(unsigned i=0;i<8;i++)fprintf(out,"%s%u",i?",":"",g_cpu.D[i]);fprintf(out,"],\"a\":[");
    for(unsigned i=0;i<8;i++)fprintf(out,"%s%u",i?",":"",g_cpu.A[i]);
    fprintf(out,"]},\"cycles\":%llu,\"pending_cycles\":%u,\"audio_cycles\":%u,\"instructions\":%llu,\"irq\":%d,\"fault\":%d,\"pending_irq\":%u,\"frame\":%llu,\"ram\":[",(unsigned long long)g_total_cycles,g_cycle_accumulator,g_audio_cycle_counter,(unsigned long long)g_native_insn_count,interrupted,fault,recomp_pending_irq_mask(),(unsigned long long)g_frame_count);
    hash(out,g_ram0,CDI_RAM0_SIZE);fputc(',',out);hash(out,g_ram1,CDI_RAM1_SIZE);
    CdiServiceJobState jobs;cdi_service_job_state(&jobs);
    fprintf(out,"],\"jobs\":{\"jobs\":%llu,\"errors\":%llu,\"interrupted\":%llu,\"fallbacks\":%llu},\"reference_frontiers\":[",(unsigned long long)jobs.jobs,(unsigned long long)jobs.errors,(unsigned long long)jobs.interrupted,(unsigned long long)jobs.fallbacks);
    int comma=0;for(uint64_t seq=0;seq<debug_trace_sequence();seq++){
        uint32_t pc,nextpc;uint64_t cycles,after;
        if(!debug_trace_point(seq,&pc,&cycles)||pc<0x40829A||pc>=0x4082C0)continue;
        if(!debug_trace_point(seq+1,&nextpc,&after))after=g_total_cycles;
        fprintf(out,"%s[%u,%llu,%llu]",comma?",":"",pc,(unsigned long long)cycles,(unsigned long long)after);comma=1;
    }
    fprintf(out,"]}\n");fclose(out);return 0;
}
