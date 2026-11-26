#include "cdi_vector_gateway.h"
#include "cdi_runtime.h"
#include "debug_server.h"
#include <stddef.h>
extern uint8_t g_ram0[],g_ram1[];
extern int game_dispatch_has_addr(uint32_t address);
static const uint8_t *ram(uint32_t address,unsigned size) {
    if(address<CDI_RAM0_SIZE && size<=CDI_RAM0_SIZE-address)return g_ram0+address;
    if(address>=CDI_RAM1_BASE && address-CDI_RAM1_BASE<CDI_RAM1_SIZE && size<=CDI_RAM1_SIZE-(address-CDI_RAM1_BASE))return g_ram1+address-CDI_RAM1_BASE;
    return NULL;
}
static uint16_t be16(const uint8_t *p){return (uint16_t)((p[0]<<8)|p[1]);}
static uint32_t be32(const uint8_t *p){return ((uint32_t)be16(p)<<16)|be16(p+2);}
static int identify(uint32_t address,uint32_t *base,uint32_t *target,int *push) {
    if(address&1u)return 0;
    const uint8_t *p=ram(address,10);*push=1;*base=address;
    if(!p || be16(p)!=0x4878u) {
        if(address<4)return 0;*base=address-4;*push=0;p=ram(*base,10);
    }
    if(!p || be16(p)!=0x4878u || be16(p+4)!=0x4ef9u)return 0;
    *target=be32(p+6);
    return *target>=CDI_ROM_BASE && *target-CDI_ROM_BASE<CDI_ROM_SIZE &&
        !(*target&1u) && game_dispatch_has_addr(*target);
}
int cdi_vector_gateway_has_address(uint32_t address) {uint32_t b,t;int push;return identify(address,&b,&t,&push);}
static void account(unsigned cycles) {
    g_native_insn_count++;g_cycle_accumulator+=cycles;g_audio_cycle_counter+=cycles;
    if(g_cycle_accumulator>=g_vblank_threshold)glue_check_vblank();
}
int cdi_vector_gateway_dispatch(uint32_t address) {
    uint32_t base,target;int push;
    if(!identify(address,&base,&target,&push))return 0;
    if(push) {
        g_cpu.PC=base;debug_trace_block();
        /* Re-read after the safepoint: a paused guarded mod may replace bytes. */
        if(!identify(base,&base,&target,&push))return 0;
        uint32_t vector=(uint32_t)(int32_t)(int16_t)be16(ram(base+2,2));
        g_cpu.A[7]-=4;m68k_write32(g_cpu.A[7],vector);
        account(25); /* SCC68070 PEA absolute-word: 13 + 12 EA periods. */
    }
    g_cpu.PC=base+4;debug_trace_block();
    uint32_t checked_base,checked_target;int checked_push;
    if(!identify(base+4,&checked_base,&checked_target,&checked_push))return 0;
    account(18); /* SCC68070 JMP absolute-long: 6 + 12 EA periods. */
    recomp_tail_call(checked_target);
    g_call_was_hybrid=0;return 1;
}
