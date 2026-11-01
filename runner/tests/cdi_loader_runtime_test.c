/* Run the actual generated firmware and the production native job from the
 * same caller state, including timer-triggered interrupt/bus-error boundaries.
 * tools/loader_runtime_fixture.py links this against supported runtime objects. */
#include "cdi_runtime.h"
#include "cdi_loader_jobs.h"
#include "cdi_sha256.h"
#include "debug_server.h"
#include "cdi_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint8_t g_ram0[CDI_RAM0_SIZE],g_ram1[CDI_RAM1_SIZE],g_rom[CDI_ROM_SIZE];
extern void cdi_bus_load_rom(const uint8_t *,uint32_t);
extern void mcd212_reset(void);
extern void runtime_reset_cpu_interrupt_state(void);
extern uint64_t g_stop_seq,g_stop_frame;
static uint32_t random_state;
static uint32_t random32(void){random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static void hash(FILE *f,const uint8_t *data,size_t count){uint8_t digest[32];cdi_sha256(data,count,digest);fputc('"',f);for(unsigned i=0;i<32;i++)fprintf(f,"%02x",digest[i]);fputc('"',f);}
int main(int argc,char **argv) {
    if(argc!=8)return 2;
    FILE *rom=fopen(argv[1],"rb");if(!rom)return 2;
    uint8_t *raw=calloc(CDI_ROM_SIZE,1);if(!raw)return 2;
    size_t bytes=fread(raw,1,CDI_ROM_SIZE,rom);fclose(rom);cdi_bus_load_rom(raw,(uint32_t)bytes);free(raw);
    int native=!strcmp(argv[3],"native");unsigned words=(unsigned)strtoul(argv[4],NULL,0);
    unsigned irq_cycle=(unsigned)strtoul(argv[5],NULL,0);random_state=(uint32_t)strtoul(argv[6],NULL,0);
    unsigned variant=(unsigned)strtoul(argv[7],NULL,0);
    for(unsigned i=0;i<CDI_RAM0_SIZE;i++){g_ram0[i]=(uint8_t)random32();g_ram1[i]=(uint8_t)random32();}
    cdi_media_init();runtime_init();runtime_reset_cpu_interrupt_state();periph_reset();nvram_reset();cdi_input_reset();mcd212_reset();
    g_total_cycles=0;g_frame_count=0;g_native_insn_count=0;g_cycle_accumulator=0;g_audio_cycle_counter=0;g_vblank_threshold=1;
    g_stop_seq=native?0:UINT64_MAX;g_stop_frame=0;g_hold_on_fault=0;
    for(unsigned i=0;i<8;i++){g_cpu.D[i]=random32();g_cpu.A[i]=random32();}
    g_cpu.D[1]=random32();g_cpu.D[3]=words-1;g_cpu.D[5]=0x800021;
    g_cpu.A[0]=0x210000+(variant==1?1u:0u);g_cpu.A[4]=0x1000;g_cpu.A[7]=0x70000;
    g_cpu.SR=(uint16_t)(0x2000|(random32()&31));g_cpu.SSP=g_cpu.A[7];g_cpu.USP=0x60000;
    g_ram0[0x101C]=0; /* Scheduling-disabled process fixture; real yields are exercised by built-game QA. */
    g_cpu.PC=0x406BF4;
    if(variant==2){g_rom[0x6BC0]^=1;cdi_loader_reset();} /* Unsupported BIOS keeps the original path. */
    if(variant==3)g_cpu.A[0]=0x180000; /* Real bus-error path, never a fabricated checksum. */
    if(irq_cycle){
        unsigned ticks=(irq_cycle+95)/96,phase=ticks*96-irq_cycle;
        periph_increment_timer(phase);
        periph_write(0x80002022,0,2);periph_write(0x80002024,(0x10000u-ticks)&0xFFFFu,2);
        periph_write(0x80002045,5,1);
        m68k_write32((56u+5u)*4u,0x4004B8);
    }
    int interrupted=0,bus_error=0;
    g_recomp_bus_armed=1;g_recomp_irq_armed=1;
    if(setjmp(g_recomp_bus_env))bus_error=1;
    else if(setjmp(g_recomp_irq_env))interrupted=1;
    else recomp_call_addr(0x406BF4);
    g_recomp_bus_armed=g_recomp_irq_armed=0;
    FILE *out=fopen(argv[2],"wb");if(!out)return 2;
    fprintf(out,"{\"cpu\":{\"pc\":%u,\"sr\":%u,\"usp\":%u,\"ssp\":%u,\"d\":[",g_cpu.PC,g_cpu.SR,g_cpu.USP,g_cpu.SSP);
    for(unsigned i=0;i<8;i++)fprintf(out,"%s%u",i?",":"",g_cpu.D[i]);
    fprintf(out,"],\"a\":[");for(unsigned i=0;i<8;i++)fprintf(out,"%s%u",i?",":"",g_cpu.A[i]);
    fprintf(out,"]},\"cycles\":%llu,\"pending_cycles\":%u,\"audio_cycles\":%u,\"frame\":%llu,\"native_insns\":%llu,\"irq\":%d,\"bus_error\":%d,\"ram\":[",(unsigned long long)g_total_cycles,g_cycle_accumulator,g_audio_cycle_counter,(unsigned long long)g_frame_count,(unsigned long long)g_native_insn_count,interrupted,bus_error);
    hash(out,g_ram0,sizeof g_ram0);fputc(',',out);hash(out,g_ram1,sizeof g_ram1);fprintf(out,"]");
    CdiLoaderJobState jobs;cdi_loader_job_state(&jobs);
    fprintf(out,",\"jobs\":{\"spans\":%llu,\"words\":%llu,\"interrupted\":%llu}}\n",(unsigned long long)jobs.spans,(unsigned long long)jobs.words,(unsigned long long)jobs.interrupted);fclose(out);
    return 0;
}
