/* Actual generated 490a rectangle fill versus the production native job.
 * RAM is isolated retained input; IRQ/fault unwinds stop at real frontiers. */
#include "cdi_runtime.h"
#include "cdi_memory_jobs.h"
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
static int observe_after_write;
static uint32_t random_state;
static uint32_t random32(void){random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
void __real_glue_check_vblank(void);
void __wrap_glue_check_vblank(void) {
    __real_glue_check_vblank();
    if(observe_after_write && g_cpu.PC==0x446206) {
        observe_after_write=0;g_stop_seq=UINT64_MAX;
        atomic_fetch_add_explicit(&g_debug_native_observation_epoch,1,memory_order_release);
    }
}
static int load(const char *path,uint8_t *p,size_t size){FILE *f=fopen(path,"rb");if(!f)return 0;size_t n=fread(p,1,size,f);fclose(f);return n==size;}
static void hash(FILE *f,const void *p,size_t size){uint8_t digest[32];cdi_sha256(p,size,digest);fputc('"',f);for(unsigned i=0;i<32;i++)fprintf(f,"%02x",digest[i]);fputc('"',f);}
int main(int argc,char **argv) {
    if(argc!=12)return 2;
    FILE *rom=fopen(argv[1],"rb");if(!rom)return 2;
    uint8_t *raw=calloc(CDI_ROM_SIZE,1);if(!raw)return 2;
    size_t n=fread(raw,1,CDI_ROM_SIZE,rom);fclose(rom);cdi_bus_load_rom(raw,(uint32_t)n);free(raw);
    if(!load(argv[2],g_ram0,CDI_RAM0_SIZE)||!load(argv[3],g_ram1,CDI_RAM1_SIZE))return 2;
    int native=!strcmp(argv[5],"native");uint32_t width=(uint32_t)strtoul(argv[6],NULL,0);
    uint32_t rows=(uint32_t)strtoul(argv[7],NULL,0),pitch=(uint32_t)strtoul(argv[8],NULL,0);
    unsigned variant=(unsigned)strtoul(argv[9],NULL,0),irq_cycle=(unsigned)strtoul(argv[10],NULL,0);
    random_state=(uint32_t)strtoul(argv[11],NULL,0);
    cdi_media_init();runtime_init();runtime_reset_cpu_interrupt_state();periph_reset();nvram_reset();cdi_input_reset();mcd212_reset();
    g_total_cycles=g_frame_count=g_native_insn_count=0;g_cycle_accumulator=g_audio_cycle_counter=0;g_vblank_threshold=1;
    g_stop_seq=native?0:UINT64_MAX;g_stop_frame=0;g_hold_on_fault=0;
    for(unsigned i=0;i<8;i++){g_cpu.D[i]=random32();g_cpu.A[i]=random32();}
    const uint32_t sp=0x70000;g_cpu.A[7]=sp;g_cpu.PC=0x445200;
    g_cpu.D[0]=355988;g_cpu.D[1]=width;g_cpu.SR=(uint16_t)(0x2000|(random32()&31));g_cpu.SSP=sp;g_cpu.USP=0x60000;
    m68k_write32(sp,0x445204);m68k_write32(sp+4,rows);m68k_write32(sp+8,4);m68k_write32(sp+12,pitch);
    if(variant==1)g_cpu.D[0]=CDI_RAM0_SIZE-4;
    if(variant==2)g_cpu.D[0]=sp-20;
    if(variant==3){if(!cdi_native_has_address(0x25F2C0+0x12A84))return 3;g_cpu.D[0]=0x25F2C0+0x100;}
    if(variant==4){g_rom[0x461EC]^=1;cdi_memory_reset();}
    if(variant==5)g_cpu.PC=0x4461EC;
    if(variant==6)g_stop_seq=UINT64_MAX;
    if(variant==7)observe_after_write=1;
    if(variant==8)m68k_write32(sp+8,0);
    if(variant==9)m68k_write32(sp+8,0x80);
    if(variant==10)g_cpu.D[0]=CDI_RAM1_BASE+0x1000;
    if(variant==11)g_cpu.D[0]++;
    if(irq_cycle){unsigned ticks=(irq_cycle+95)/96,phase=ticks*96-irq_cycle;
        periph_increment_timer(phase);periph_write(0x80002022,0,2);periph_write(0x80002024,(0x10000u-ticks)&0xFFFFu,2);
        periph_write(0x80002045,5,1);m68k_write32((56u+5u)*4u,0x4004B8);
    }
    RecompFuncPtr entry=recomp_native_find_entry(0x4461EC);if(!entry)return 4;
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
    CdiNativeState binding;cdi_native_state(&binding);
    fprintf(out,"],\"bindings\":[%llu,%llu,%u,%u],\"events\":[",(unsigned long long)binding.bindings,(unsigned long long)binding.invalidations,binding.active,binding.compiled);
    CdiNativeEvent events[16];int count=cdi_native_events(events,16,0,NULL,NULL);
    for(int i=0;i<count;i++)fprintf(out,"%s[%u,%u,%u,%u,%llu,%llu]",i?",":"",events[i].type,events[i].pc,events[i].base,events[i].module,(unsigned long long)events[i].cycles,(unsigned long long)events[i].frame);
    CdiMemoryJobState jobs;cdi_memory_job_state(&jobs);
    fprintf(out,"],\"jobs\":{\"jobs\":%llu,\"bytes\":%llu,\"rows\":%llu,\"interrupted\":%llu,\"fallbacks\":%llu},\"reference_frontiers\":[",(unsigned long long)jobs.jobs,(unsigned long long)jobs.bytes,(unsigned long long)jobs.rows,(unsigned long long)jobs.interrupted,(unsigned long long)jobs.fallbacks);
    int comma=0;uint32_t seen=0;for(uint64_t seq=0;seq<debug_trace_sequence();seq++){
        uint32_t pc,nextpc;uint64_t cycles,after;
        if(!debug_trace_point(seq,&pc,&cycles)||pc<0x4461EC||pc>=0x44621C)continue;
        uint32_t bit=1u<<((pc-0x4461EC)/2);if(seen&bit)continue;seen|=bit;
        if(!debug_trace_point(seq+1,&nextpc,&after))after=g_total_cycles;
        fprintf(out,"%s[%u,%llu,%llu]",comma?",":"",pc,(unsigned long long)cycles,(unsigned long long)after);comma=1;
    }
    fprintf(out,"]}\n");fclose(out);return 0;
}
