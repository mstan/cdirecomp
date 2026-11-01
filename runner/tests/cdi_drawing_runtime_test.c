/* Actual generated drawing service vs production native job, in an isolated
 * copy of the retained built-game RAM. No guest progress is written in games. */
#include "cdi_runtime.h"
#include "cdi_native.h"
#include "cdi_drawing_jobs.h"
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
extern uint32_t cdi_module_000_base;
extern uint64_t g_stop_seq,g_stop_frame;
static uint32_t rand_state;
static int sync_each_clock;
static uint32_t debug_clock_gate;
void __real_glue_check_vblank(void);
void __wrap_glue_check_vblank(void) {
    __real_glue_check_vblank();
    /* Exercise repeated debugger materialization followed by continuing the
     * same native segment. The reference has already materialized its CPU. */
    if(sync_each_clock)cdi_drawing_sync_cpu();
    if(debug_clock_gate && g_cpu.PC==debug_clock_gate) {
        debug_clock_gate=0;
        puts("drawing-clock-ready");fflush(stdout);
        if(getchar()==EOF)exit(5);
    }
}
static uint32_t random32(void){rand_state^=rand_state<<13;rand_state^=rand_state>>17;rand_state^=rand_state<<5;return rand_state;}
static void put(uint32_t a,uint32_t v){m68k_write32(a,v);}
static int load(const char *path,uint8_t *p,size_t size){FILE *f=fopen(path,"rb");if(!f)return 0;size_t n=fread(p,1,size,f);fclose(f);return n==size;}
static void hash(FILE *f,const uint8_t *p,size_t n){uint8_t d[32];cdi_sha256(p,n,d);fputc('"',f);for(unsigned i=0;i<32;i++)fprintf(f,"%02x",d[i]);fputc('"',f);}
int main(int argc,char **argv){
    if(argc!=12 && argc!=14 && argc!=15)return 2;
    uint8_t *raw=calloc(CDI_ROM_SIZE,1);FILE *rom=fopen(argv[1],"rb");if(!raw||!rom)return 2;
    size_t rom_bytes=fread(raw,1,CDI_ROM_SIZE,rom);fclose(rom);
    cdi_bus_load_rom(raw,(uint32_t)rom_bytes);free(raw);
    if(!load(argv[2],g_ram0,CDI_RAM0_SIZE)||!load(argv[3],g_ram1,CDI_RAM1_SIZE))return 2;
    int native=!strcmp(argv[5],"native");int32_t budget=(int32_t)strtol(argv[6],NULL,0);
    unsigned queued=(unsigned)strtoul(argv[7],NULL,0),variant=(unsigned)strtoul(argv[8],NULL,0);
    unsigned irq_cycle=(unsigned)strtoul(argv[9],NULL,0);rand_state=(uint32_t)strtoul(argv[10],NULL,0);
    unsigned wrap=(unsigned)strtoul(argv[11],NULL,0);
    sync_each_clock=variant==6;
    cdi_media_init();runtime_init();runtime_reset_cpu_interrupt_state();periph_reset();nvram_reset();cdi_input_reset();mcd212_reset();
    g_total_cycles=g_frame_count=g_native_insn_count=0;g_cycle_accumulator=g_audio_cycle_counter=0;g_vblank_threshold=1;
    g_stop_seq=native?0:UINT64_MAX;g_stop_frame=0;g_hold_on_fault=0;
    for(unsigned i=0;i<8;i++){g_cpu.D[i]=random32();g_cpu.A[i]=random32();}
    const uint32_t base=0x25F2C0;uint32_t sp=0x7F000;
    g_cpu.D[0]=(uint32_t)budget;g_cpu.A[6]=0xC000;g_cpu.A[7]=sp;g_cpu.A[5]=0x70000;
    g_cpu.SR=(uint16_t)(0x2000|(random32()&31));g_cpu.SSP=sp;g_cpu.USP=0x60000;
    g_cpu.PC=base+0x12A84;cdi_module_000_base=base;
    put(sp,base+0x12A88);
    uint32_t start=0xCBA0,end=start+15*8,tail=wrap?end:start,head=tail;
    for(unsigned i=0;i<queued;i++){head+=8;if(head>end)head=start;}
    put(0x51A6,start);put(0x51AA,end);put(0x51AE,head);put(0x51B2,tail);
    uint32_t callback=base+0x19E4;
    if(variant==2){callback=0x1800;m68k_write16(callback,0x23FC);put(callback+2,0x12345678);put(callback+6,base+0x100);m68k_write16(callback+10,0x4E75);}
    if(variant==3){callback=0x1800;m68k_write16(callback,0x78FF);m68k_write16(callback+2,0x4E75);}
    for(uint32_t a=start;a<=end;a+=8){put(a,callback);put(a+4,0x7CB10);}
    if(variant==1){
        uint32_t object=m68k_read32(0x4310),descriptor=m68k_read32(object+8);
        m68k_write8(object+0x8C8,0);put(descriptor+10,0x11000);
        put(0x4034,0x10000);put(0x4038,0x10480);put(0x403C,0);put(0x4040,0);
        for(uint32_t a=0x10000;a<0x11900;a++)m68k_write8(a,(uint8_t)random32());
    }
    if(variant==4){put(0x51B2,0x180000);put(0x51AE,start+8);}
    if(!cdi_native_has_address(base+0x12A84))return 3;
    if(variant==5){g_cpu.A[7]=base+112984;put(g_cpu.A[7],base+0x12A88);}
    if(irq_cycle){unsigned ticks=(irq_cycle+95)/96,phase=ticks*96-irq_cycle;
        periph_increment_timer(phase);periph_write(0x80002022,0,2);periph_write(0x80002024,(0x10000u-ticks)&0xFFFFu,2);
        periph_write(0x80002045,5,1);put((56u+5u)*4u,0x4004B8);
    }
    CdiNativeFunction entry=cdi_native_find_function(g_cdi_native_modules[0].sha256,0x17018,NULL);
    if(!entry)return 4;
    if(argc>=14) {
        debug_server_set_session(argv[13]);debug_server_init(atoi(argv[12]));
        if(argc==15)debug_clock_gate=(uint32_t)strtoul(argv[14],NULL,0);
        puts("drawing-debug-ready");fflush(stdout);
        if(getchar()==EOF)return 5;
    }
    uint8_t before[32];cdi_sha256(g_ram0+0x11000,0x900,before);
    int interrupted=0,bus_error=0;g_recomp_bus_armed=g_recomp_irq_armed=1;
    if(setjmp(g_recomp_bus_env))bus_error=1;
    else if(setjmp(g_recomp_irq_env))interrupted=1;
    else recomp_call_func(entry);
    g_recomp_bus_armed=g_recomp_irq_armed=0;
    uint8_t after[32];cdi_sha256(g_ram0+0x11000,0x900,after);
    FILE *out=fopen(argv[4],"wb");if(!out)return 2;
    fprintf(out,"{\"cpu\":{\"pc\":%u,\"sr\":%u,\"usp\":%u,\"ssp\":%u,\"d\":[",g_cpu.PC,g_cpu.SR,g_cpu.USP,g_cpu.SSP);
    for(unsigned i=0;i<8;i++)fprintf(out,"%s%u",i?",":"",g_cpu.D[i]);fprintf(out,"],\"a\":[");
    for(unsigned i=0;i<8;i++)fprintf(out,"%s%u",i?",":"",g_cpu.A[i]);
    fprintf(out,"]},\"cycles\":%llu,\"pending\":%u,\"audio_cycles\":%u,\"instructions\":%llu,\"frame\":%llu,\"irq\":%d,\"bus_error\":%d,\"redirect\":[%d,%u],\"rte\":[%d,%d],\"painted\":%d,\"ram\":[",(unsigned long long)g_total_cycles,g_cycle_accumulator,g_audio_cycle_counter,(unsigned long long)g_native_insn_count,(unsigned long long)g_frame_count,interrupted,bus_error,g_redirect_pending,g_redirect_addr,g_rte_pending,g_rte_resume,memcmp(before,after,32)!=0);
    hash(out,g_ram0,CDI_RAM0_SIZE);fputc(',',out);hash(out,g_ram1,CDI_RAM1_SIZE);
    CdiDrawingJobState jobs;cdi_drawing_job_state(&jobs);
    fprintf(out,"],\"jobs\":{\"jobs\":%llu,\"callbacks\":%llu,\"empty\":%llu,\"interrupts\":%llu,\"fallbacks\":%llu},\"reference_frontiers\":[",(unsigned long long)jobs.jobs,(unsigned long long)jobs.callbacks,(unsigned long long)jobs.empty_checks,(unsigned long long)jobs.interrupted,(unsigned long long)jobs.fallbacks);
    int comma=0;
    for(uint64_t seq=0;seq<debug_trace_sequence();seq++){
        uint32_t pc,nextpc;uint64_t cycles,after_cycles;
        if(!debug_trace_point(seq,&pc,&cycles))continue;
        if(pc<base+0x17018 || pc>=base+0x17076)continue;
        if(!debug_trace_point(seq+1,&nextpc,&after_cycles))after_cycles=g_total_cycles;
        fprintf(out,"%s{\"pc\":%u,\"entry_cycles\":%llu,\"after_cycles\":%llu}",comma?",":"",pc,(unsigned long long)cycles,(unsigned long long)after_cycles);comma=1;
    }
    fprintf(out,"]}\n");fclose(out);return 0;
}
