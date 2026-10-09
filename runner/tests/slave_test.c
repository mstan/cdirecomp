#include "cdi_runtime.h"
#include "debug_server.h"
#include <stdint.h>
#include <stdio.h>

M68KState g_cpu;
uint64_t g_total_cycles, g_frame_count;
static int failures, irqs;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); failures++; } } while (0)
uint64_t debug_trace_sequence(void) { return 0; }
void cdi_irq_raise(uint8_t level) { CHECK(level==2);irqs++; }
void cdi_request_main_cpu_boot_reset(void) {}
void cdi_request_main_cpu_reset(void) {}
int cdi_media_present(void) { return 1; }
uint64_t cdi_media_generation(void) { return 1; }
void cdic_transport_resume(void) {}
void cdic_set_drive_position(uint32_t lba,int audio) { (void)lba;(void)audio; }
static void command(uint8_t op) {
    const uint8_t bytes[4]={op,0,2,0};
    for (int i=0;i<4;i++) slave_write(CDI_SLAVE_BASE+7,bytes[i],1);
}
static int remaining(void) {
    uint8_t registers[15], lengths[4];double ns;int packets;
    slave_debug_state(registers,lengths,&ns,&packets);
    return lengths[3];
}
static void response(uint8_t op,uint8_t status) {
    CHECK(slave_read(CDI_SLAVE_BASE+15,1)==op);
    CHECK(slave_read(CDI_SLAVE_BASE+15,1)==0);
    CHECK(slave_read(CDI_SLAVE_BASE+15,1)==2);
    CHECK(slave_read(CDI_SLAVE_BASE+15,1)==status);
}
int main(void) {
    slave_write(CDI_SLAVE_BASE+27,0x80,1);
    command(0xb0);
    CHECK(remaining()==0 && irqs==0);
    slave_increment_time(999999.0);
    CHECK(remaining()==0 && irqs==0);
    slave_increment_time(1.0);
    CHECK(remaining()==4 && irqs>0 && g_frame_count==0);
    response(0xb0,0x15);
    /* Busy and unsolicited on-target messages must survive independently,
     * even if the host delays reading past the second response's deadline. */
    command(0xe1);
    command(0xb0);
    slave_increment_time(1000000.0);
    CHECK(remaining()==4);
    slave_increment_time(5000000.0);
    response(0xb0,0x10);
    CHECK(remaining()==0);
    slave_increment_time(1.0);
    CHECK(remaining()==4);
    response(0xb0,0x0e);
    return failures?1:0;
}
