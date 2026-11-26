#include "cdi_vector_gateway.h"
#include "cdi_runtime.h"
#include <string.h>
#include <stdio.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed at %d: %s\n",__LINE__,#x);return 1;}}while(0)
uint8_t g_ram0[CDI_RAM0_SIZE],g_ram1[CDI_RAM1_SIZE];M68KState g_cpu;
uint64_t g_native_insn_count;uint32_t g_cycle_accumulator,g_audio_cycle_counter,g_vblank_threshold=1000;int g_call_was_hybrid;
static uint32_t trace_pc[4],trace_count,tail,stored,stored_address;
void debug_trace_block(void){trace_pc[trace_count++]=g_cpu.PC;}
void glue_check_vblank(void){}
void m68k_write32(uint32_t a,uint32_t v){stored_address=a;stored=v;}
void recomp_tail_call(uint32_t a){tail=a;}
int game_dispatch_has_addr(uint32_t a){return a==0x4006a2;}
int main(void) {
    const uint8_t thunk[]={0x48,0x78,0x00,0x0c,0x4e,0xf9,0x00,0x40,0x06,0xa2};
    memcpy(g_ram0+0x50a,thunk,sizeof thunk);g_cpu.A[7]=0x1500;g_cpu.SR=0x270f;
    CHECK(cdi_vector_gateway_has_address(0x50a));CHECK(cdi_vector_gateway_has_address(0x50e));
    CHECK(cdi_vector_gateway_dispatch(0x50a));
    CHECK(stored_address==0x14fc && stored==12 && g_cpu.A[7]==0x14fc && g_cpu.SR==0x270f);
    CHECK(trace_count==2 && trace_pc[0]==0x50a && trace_pc[1]==0x50e && tail==0x4006a2);
    CHECK(g_native_insn_count==2 && g_cycle_accumulator==43 && g_audio_cycle_counter==43);
    trace_count=0;g_native_insn_count=0;g_cycle_accumulator=0;g_audio_cycle_counter=0;
    CHECK(cdi_vector_gateway_dispatch(0x50e));CHECK(g_cpu.A[7]==0x14fc && trace_count==1 && g_native_insn_count==1 && g_cycle_accumulator==18);
    g_ram0[0x50b]=0x79;CHECK(!cdi_vector_gateway_has_address(0x50a));
    CHECK(!cdi_vector_gateway_dispatch(0x50a));CHECK(!cdi_vector_gateway_has_address(0x50b));
    puts("Native OS-9 vector gateway: guarded shape/target, stack, flags, cycles and interior resume passed");return 0;
}
