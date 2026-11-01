/* Production whole-board STOP job versus the original individual tick25 path.
 * All setup is isolated. IRQ/pacing hashes use actual device event cycles. */
#include "cdi_runtime.h"
#include "cdi_board_jobs.h"
#include "cdi_streaming_jobs.h"
#include "cdi_media.h"
#include "cdi_audio.h"
#include "cdi_sha256.h"
#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint8_t g_ram0[CDI_RAM0_SIZE],g_ram1[CDI_RAM1_SIZE];
extern void cdi_bus_load_rom(const uint8_t *,uint32_t);
extern void runtime_reset_cpu_interrupt_state(void);
extern void mcd212_reset(void);
extern uint64_t g_stop_seq;
static uint64_t timeline=UINT64_C(0xcbf29ce484222325),pcm=UINT64_C(0xcbf29ce484222325),pace_pending;
static unsigned pump_count,variant;
static CdiBoardJobState earlier_jobs;
static const char *disc;
static void bytes(uint64_t *hash,const void *data,size_t size){const uint8_t *p=data;while(size--){*hash^=*p++;*hash*=UINT64_C(0x100000001b3);}}
static void number(uint64_t value){bytes(&timeline,&value,sizeof value);}
void __real_cdi_irq_raise(uint8_t);
void __wrap_cdi_irq_raise(uint8_t level){number(1);number(g_total_cycles);number(level);__real_cdi_irq_raise(level);}
void __real_cdi_irq_raise_vector(uint8_t,uint8_t);
void __wrap_cdi_irq_raise_vector(uint8_t level,uint8_t vector){number(2);number(g_total_cycles);number(level);number(vector);__real_cdi_irq_raise_vector(level,vector);}
void __real_cdi_irq_raise_onchip_level(uint8_t);
void __wrap_cdi_irq_raise_onchip_level(uint8_t level){number(3);number(g_total_cycles);number(level);__real_cdi_irq_raise_onchip_level(level);}
void __real_cdi_irq_clear(uint8_t);
void __wrap_cdi_irq_clear(uint8_t level){number(4);number(g_total_cycles);number(level);__real_cdi_irq_clear(level);}
void __real_runtime_pace_cycles(uint32_t);
void __wrap_runtime_pace_cycles(uint32_t cycles){pace_pending+=cycles;if(pace_pending>=151049u){number(5);number(g_total_cycles);number(pace_pending);pace_pending=0;}__real_runtime_pace_cycles(cycles);}
void __real_glue_check_vblank(void);
void __wrap_glue_check_vblank(void){__real_glue_check_vblank();}
static int pump(void) {
    pump_count++;
    if(variant==1 && pump_count==100){g_stop_seq=UINT64_MAX;atomic_fetch_add_explicit(&g_debug_native_observation_epoch,1,memory_order_release);}
    if(variant==1 && pump_count==7000){g_stop_seq=0;atomic_fetch_add_explicit(&g_debug_native_observation_epoch,1,memory_order_release);}
    if(variant==2 && pump_count==100)cdi_media_eject();
    if(variant==2 && pump_count==300 && !cdi_media_mount(disc))exit(3);
    return !(variant==3 && pump_count==73219);
}
static void write_reg(unsigned offset,uint16_t value){cdic_write(CDI_CDIC_BASE+offset,value,2);}
static void refill(unsigned buffer,const uint8_t *groups) {
    memcpy(g_ram0+0x50000,groups,2304);
    periph_write(0x80004007,0x80,1);periph_write(0x8000400A,1152,2);periph_write(0x8000400C,0x50000,4);
    write_reg(0x25C2,(uint16_t)(0x4000u+buffer*0x480u));write_reg(0x259A,0x0800);write_reg(0x2594,(uint16_t)(1u<<buffer));
}
static void checkpoint(void) {
    uint16_t regs[32],line;uint8_t status,irq;uint32_t raster;
    mcd212_debug_state(regs,&status,&irq,&raster,&line);
    number(g_total_cycles);number(g_frame_count);number(raster);number(line);number(status);number(irq);bytes(&timeline,regs,sizeof regs);
    for(uint32_t a=0x80002020;a<0x8000202a;a++)number(periph_read(a,1));
    uint8_t registers[15],remaining[4];double pointer_time;int packets;
    slave_debug_state(registers,remaining,&pointer_time,&packets);bytes(&timeline,registers,sizeof registers);bytes(&timeline,remaining,sizeof remaining);
    bytes(&timeline,&pointer_time,sizeof pointer_time);number((unsigned)packets);
    for(unsigned channel=0;channel<4;channel++)for(unsigned i=0;i<remaining[channel];i++)number(slave_read(CDI_SLAVE_BASE+2u*(4u+channel),1));
    cdi_streaming_sync();for(unsigned a=0;a<0x2600;a+=2)number(cdic_read(CDI_CDIC_BASE+a,2));
    periph_write(0x80004007,0x80,1);periph_write(0x8000400A,37,2);periph_write(0x8000400C,0x60000,4);write_reg(0x25C2,0x6900);
    write_reg(0x2594,0x003C);
    int16_t samples[8192*2];uint32_t count;
    do {count=cdi_audio_read_frames(samples,8192);bytes(&pcm,samples,count*2u*sizeof samples[0]);}while(count);
    number(recomp_pending_irq_mask());
    runtime_reset_cpu_interrupt_state();g_halted=1;
}
static void rtc_snapshot(void) {
    static const uint8_t unlock[]={0xC5,0x3A,0xA3,0x5C,0xC5,0x3A,0xA3,0x5C};
    for(unsigned i=0;i<8;i++)for(unsigned bit=0;bit<8;bit++)nvram_set_byte(0,(unlock[i]>>bit)&1u);
    for(unsigned i=0;i<8;i++){unsigned value=0;for(unsigned bit=0;bit<8;bit++)value|=(nvram_get_byte(0)&1u)<<bit;number(value);}
}
static void hash(FILE *out,const void *data,size_t size){uint8_t digest[32];cdi_sha256(data,size,digest);fputc('"',out);for(unsigned i=0;i<32;i++)fprintf(out,"%02x",digest[i]);fputc('"',out);}
int main(int argc,char **argv) {
    if(argc!=6)return 2;
    disc=argv[2];variant=(unsigned)strtoul(argv[5],NULL,0);int native=!strcmp(argv[4],"native");
    FILE *file=fopen(argv[1],"rb");if(!file)return 2;uint8_t *rom=calloc(CDI_ROM_SIZE,1);if(!rom)return 2;
    size_t n=fread(rom,1,CDI_ROM_SIZE,file);fclose(file);cdi_bus_load_rom(rom,(uint32_t)n);free(rom);
    cdi_media_init();runtime_init();runtime_reset_cpu_interrupt_state();periph_reset();nvram_reset();cdi_input_reset();mcd212_reset();
    if(!cdi_media_mount(disc))return 3;
    runtime_set_realtime_pacing(1);g_halted=1;g_stop_seq=0;g_total_cycles=0;g_cpu.PC=0x4077DE;g_cpu.SR=0x2000;
    cdi_input_schedule_configure("2:16,4:0,6:4,9:0");
    slave_write(CDI_SLAVE_BASE+26,variant?0:0x0f,1);
    periph_write(0x80002022,0xf000,2);periph_write(0x80002024,0xfefd,2);
    periph_write(0x80002026,0xffef,2);periph_write(0x80002028,0xfefd,2);
    periph_write(0x80002021,variant?0x30:0x33,1);periph_write(0x80002045,variant?0:5,1);
    uint8_t sector[2340];if(!cdi_media_read_sector_body(42055,sector))return 3;
    write_reg(0x25C0,variant?0:0x0553);write_reg(0x2584,0x060f);write_reg(0x2592,1);write_reg(0x258C,0x1000);write_reg(0x2588,0x1000);
    cdic_set_drive_position(42040,0);write_reg(0x2596,8);write_reg(0x2596,0x44);
    refill(0,sector+12);refill(1,sector+12);write_reg(0x25A6,0x140);
    uint32_t completed=0;int quit=0;
    while(completed<240000 && !quit) {
        if(!pump()){quit=1;break;}
        unsigned maximum=4000u-completed%4000u;
        unsigned advanced=native?cdi_board_idle_quanta(maximum,pump,&quit):0;
        if(!advanced && !quit){mcd212_tick(25);advanced=1;}
        completed+=advanced;
        if(completed%4000u==0) {
            checkpoint();mcd212_tick(13u+(completed/4000u*7u)%64u);
            if(completed==80000){cdic_transport_pause();cdic_set_drive_position(42055,0);}
            if(completed==84000)cdic_transport_resume();
            if(completed==120000){cdi_board_job_state(&earlier_jobs);mcd212_reset();periph_reset();}
            if(completed==160000){write_reg(0x25A6,6);refill(1,sector+12);write_reg(0x25A6,0x40);}
        }
    }
    checkpoint();rtc_snapshot();number(completed);number(pump_count);number(quit);number(pace_pending);
    CdiAudioState audio;cdi_audio_debug_state(&audio);CdiBoardJobState jobs;cdi_board_job_state(&jobs);
    jobs.jobs+=earlier_jobs.jobs;jobs.quiet_quanta+=earlier_jobs.quiet_quanta;
    jobs.faithful_quanta+=earlier_jobs.faithful_quanta;jobs.observation_exits+=earlier_jobs.observation_exits;
    FILE *out=fopen(argv[3],"wb");if(!out)return 2;
    fprintf(out,"{\"timeline\":\"%016llx\",\"drained_pcm\":\"%016llx\",\"cycles\":%llu,\"frame\":%llu,\"quanta\":%u,\"pumps\":%u,\"quit\":%d,\"sectors\":%llu,\"pcm_frames\":%llu,\"pcm_hash\":\"%016llx\",\"dropped\":%llu,\"ram\":[",(unsigned long long)timeline,(unsigned long long)pcm,(unsigned long long)g_total_cycles,(unsigned long long)g_frame_count,completed,pump_count,quit,(unsigned long long)audio.sectors,(unsigned long long)audio.sample_frames,(unsigned long long)audio.pcm_fnv1a,(unsigned long long)audio.dropped_frames);
    hash(out,g_ram0,CDI_RAM0_SIZE);fputc(',',out);hash(out,g_ram1,CDI_RAM1_SIZE);
    fprintf(out,"],\"jobs\":{\"jobs\":%llu,\"quiet_quanta\":%llu,\"faithful_quanta\":%llu,\"observation_exits\":%llu}}\n",(unsigned long long)jobs.jobs,(unsigned long long)jobs.quiet_quanta,(unsigned long long)jobs.faithful_quanta,(unsigned long long)jobs.observation_exits);fclose(out);return 0;
}
