/* Real mounted image, production CIAP/DMA/audio and complete original backend.
 * Compare exact IRQ quantum, all sampled device bytes, DMA RAM and drained PCM;
 * the fixture changes only isolated device/guest RAM, never an owner's save. */
#include "cdi_runtime.h"
#include "cdi_media.h"
#include "cdi_audio.h"
#include "cdi_streaming_jobs.h"
#include "cdi_sha256.h"
#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint8_t g_ram0[CDI_RAM0_SIZE],g_ram1[CDI_RAM1_SIZE];
extern void cdi_bus_load_rom(const uint8_t *,uint32_t);
extern void runtime_reset_cpu_interrupt_state(void);
static uint64_t timeline=UINT64_C(0xcbf29ce484222325),pcm=UINT64_C(0xcbf29ce484222325);
static unsigned quantum,raises,clears;
static void bytes(uint64_t *hash,const void *data,size_t size) {
    const uint8_t *p=data;while(size--){*hash^=*p++;*hash*=UINT64_C(0x100000001b3);}
}
static void number(uint64_t value){bytes(&timeline,&value,sizeof value);}
void __real_cdi_irq_raise_vector(uint8_t,uint8_t);
void __wrap_cdi_irq_raise_vector(uint8_t level,uint8_t vector) {
    raises++;number(quantum);number(g_total_cycles);number(level);number(vector);
    __real_cdi_irq_raise_vector(level,vector);
}
void __real_cdi_irq_clear(uint8_t);
void __wrap_cdi_irq_clear(uint8_t level) {
    clears++;number(quantum);number(g_total_cycles);number(level);
    __real_cdi_irq_clear(level);
}
void __real_glue_check_vblank(void);
void __wrap_glue_check_vblank(void){__real_glue_check_vblank();}
static void write_reg(unsigned offset,uint16_t value){cdic_write(CDI_CDIC_BASE+offset,value,2);}
static void refill(unsigned buffer,const uint8_t *groups,uint16_t format) {
    memcpy(g_ram0+0x50000,groups,2304);
    periph_write(0x80004007,0x80,1);periph_write(0x8000400A,1152,2);
    periph_write(0x8000400C,0x50000,4);
    write_reg(0x25C2,(uint16_t)(0x4000u+buffer*0x480u));
    write_reg(0x259A,format);write_reg(0x2594,(uint16_t)(1u<<buffer));
}
static void checkpoint(void) {
    CdiCiapState state={0};cdic_debug_snapshot(&state);
    bytes(&timeline,&state,sizeof state);number(recomp_pending_irq_mask());
    number(cdic_read(CDI_CDIC_BASE+0x25B4,2)); /* fractional AP progress */
    for(unsigned offset=0;offset<0x2600;offset+=2)
        number(cdic_read(CDI_CDIC_BASE+offset,2));
    /* DMA from a genuine delivered sector, including partial transfers. */
    periph_write(0x80004007,0x80,1);periph_write(0x8000400A,37,2);
    periph_write(0x8000400C,0x60000,4);write_reg(0x25C2,0x6900);
    write_reg(0x2594,0x003C);
    int16_t samples[8192*2];uint32_t n=cdi_audio_read_frames(samples,8192);
    bytes(&pcm,samples,n*2u*sizeof samples[0]);number(n);
}
static void digest(FILE *out,const void *data,size_t size) {
    uint8_t value[32];cdi_sha256(data,size,value);fputc('"',out);
    for(unsigned i=0;i<32;i++)fprintf(out,"%02x",value[i]);fputc('"',out);
}
int main(int argc,char **argv) {
    if(argc!=5)return 2;unsigned variant=(unsigned)strtoul(argv[4],NULL,0);
    FILE *input=fopen(argv[1],"rb");if(!input)return 2;
    uint8_t *rom=calloc(CDI_ROM_SIZE,1);if(!rom)return 2;
    size_t count=fread(rom,1,CDI_ROM_SIZE,input);fclose(input);cdi_bus_load_rom(rom,(uint32_t)count);free(rom);
    cdi_media_init();runtime_init();runtime_reset_cpu_interrupt_state();periph_reset();
    if(!cdi_media_mount(argv[2]))return 3;
    uint8_t sector[2340];if(!cdi_media_read_sector_body(42055,sector) || !(sector[6]&4u))return 3;
    g_total_cycles=0;g_cpu.PC=0x428674;g_cpu.SR=0x2000;g_hold_on_fault=0;
    write_reg(0x25C0,0x0553);write_reg(0x2584,0x060F);
    write_reg(0x2592,1);write_reg(0x258C,0x1000);write_reg(0x2588,0x1000);
    cdic_set_drive_position(42040,0);write_reg(0x2596,8);write_reg(0x2596,0x44);
    refill(0,sector+12,0x0800);refill(1,sector+12,0x1800);write_reg(0x25A6,0x140);
    for(quantum=1;quantum<=240000;quantum++) {
        static const unsigned cycles[]={4,11,13,65,19,22,7};
        double ns=(double)cycles[quantum%7]*(1.0e9/15104900.0);
        if(variant==1)ns=(quantum%1000==0)?53333337.0:263.9;
        if(variant==2)ns=(quantum%997==0)?20000.0:9888.75;
        cdic_increment_time(ns);g_total_cycles+=cycles[quantum%7];
        /* Deliberately inspect IRQ visibility EVERY quantum without forcing
         * pointer publication, so delayed/early event execution cannot hide. */
        number(recomp_pending_irq_mask());
        if(quantum%4000==0)checkpoint();
        switch(quantum) {
        case 25000:cdic_transport_pause();break;
        case 30000:cdic_transport_resume();break;
        case 40000:write_reg(0x258C,0x4000);write_reg(0x2588,0x4000);write_reg(0x2596,8);break;
        case 60000:write_reg(0x2592,2);break;
        case 80000:write_reg(0x2592,1);cdic_set_drive_position(42055,0);break;
        case 100000:write_reg(0x25A6,0x20);break;
        case 120000:write_reg(0x25A6,6);refill(1,sector+12,0);write_reg(0x25A6,0x40);break;
        case 140000:write_reg(0x2596,0x100);write_reg(0x25A6,0xA0);break;
        case 150000:write_reg(0x2596,0x44);write_reg(0x2596,8);break;
        case 160000:cdi_media_eject();break;
        case 170000:if(!cdi_media_mount(argv[2]))return 3;cdic_set_drive_position(42040,0);write_reg(0x2596,0x44);break;
        case 180000:write_reg(0x258C,0x1000);write_reg(0x2588,0x1000);break;
        case 200000:refill(0,sector+12,0x0800);write_reg(0x25A6,0x140);break;
        default:break;
        }
    }
    cdic_increment_time(263.9);cdi_streaming_sync();checkpoint();
    CdiAudioState audio;cdi_audio_debug_state(&audio);CdiStreamingJobState jobs;cdi_streaming_job_state(&jobs);
    FILE *out=fopen(argv[3],"wb");if(!out)return 2;
    fprintf(out,"{\"timeline\":\"%016llx\",\"drained_pcm\":\"%016llx\",\"raises\":%u,\"clears\":%u,\"sectors\":%llu,\"frames\":%llu,\"pcm\":\"%016llx\",\"dropped\":%llu,\"ram\":[",(unsigned long long)timeline,(unsigned long long)pcm,raises,clears,(unsigned long long)audio.sectors,(unsigned long long)audio.sample_frames,(unsigned long long)audio.pcm_fnv1a,(unsigned long long)audio.dropped_frames);
    digest(out,g_ram0,CDI_RAM0_SIZE);fputc(',',out);digest(out,g_ram1,CDI_RAM1_SIZE);
    fprintf(out,"],\"jobs\":%llu,\"deferred\":%llu,\"materializations\":%llu}\n",(unsigned long long)jobs.jobs,(unsigned long long)jobs.deferred_quanta,(unsigned long long)jobs.materializations);fclose(out);return 0;
}
