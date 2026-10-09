/* Executes emitted C against a small, explicit guest bus/dispatch contract.
 * These fixture handlers do not replace any production OS-9 or hardware. */
#include "cdi_runtime.h"
#include "cdi_native.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

int fixture_dispatch(uint32_t,uint32_t);
int fixture_has_offset(uint32_t);
extern uint32_t fixture_base;
M68KState g_cpu;
uint64_t g_native_insn_count;
uint32_t g_cycle_accumulator,g_audio_cycle_counter,g_vblank_threshold=0xffffffff;
int g_redirect_pending,g_halted,g_rte_resume,g_call_was_hybrid;
uint32_t g_redirect_addr;
static int rte_pending;
int *g_rte_pending_ptr=&rte_pending;
static uint8_t memory[0x8000];
static uint32_t active_base,interrupt_pc;
static uint32_t jumped;
static int failures,traps,absolute_calls,invalid;
static jmp_buf interrupt;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);failures++; } } while (0)
uint8_t m68k_read8(uint32_t addr) { CHECK(addr<sizeof memory);return memory[addr%sizeof memory]; }
uint16_t m68k_read16(uint32_t addr) { return (m68k_read8(addr)<<8)|m68k_read8(addr+1); }
uint32_t m68k_read32(uint32_t addr) { return ((uint32_t)m68k_read16(addr)<<16)|m68k_read16(addr+2); }
void m68k_write8(uint32_t addr,uint8_t value) { CHECK(addr<sizeof memory);memory[addr%sizeof memory]=value; }
void m68k_write16(uint32_t addr,uint16_t value) { m68k_write8(addr,value>>8);m68k_write8(addr+1,value); }
void m68k_write32(uint32_t addr,uint32_t value) { m68k_write16(addr,value>>16);m68k_write16(addr+2,value); }
void recomp_push_return(uint32_t pc) { g_cpu.A[7]-=4;m68k_write32(g_cpu.A[7],pc); }
void glue_check_vblank(void) { CHECK(0); }
void runtime_defer_exception_cycles(uint32_t cycles) { CHECK(cycles==52); }
void m68k_trap_vector(uint8_t vector) {
    CHECK(vector==0x20 && g_cpu.PC==active_base+0x7c);
    CHECK(m68k_read16(g_cpu.PC)==1);traps++;
}
uint64_t cdi_native_execution_token(uint32_t base,CdiNativeDispatch expected) {
    CHECK(base==active_base && expected==fixture_dispatch);return 42;
}
int cdi_native_instruction_valid(uint32_t addr,CdiNativeDispatch expected,uint64_t token) {
    return !invalid && token==42 && expected==fixture_dispatch && addr>=active_base && addr<active_base+0x200;
}
void debug_trace_block(void) {
    CHECK(g_cpu.PC>=active_base && g_cpu.PC<active_base+0x200);
    if (interrupt_pc && g_cpu.PC==interrupt_pc) { interrupt_pc=0;longjmp(interrupt,1); }
}
void recomp_call_func(RecompFuncPtr fn) { fn();g_call_was_hybrid=0; }
void recomp_call_addr(uint32_t addr) {
    if (addr==0xb8) { absolute_calls++;g_cpu.D[7]=7;g_call_was_hybrid=0;return; }
    CHECK(addr>=active_base && fixture_dispatch(active_base,addr-active_base));
}
void recomp_tail_call(uint32_t addr) { recomp_call_addr(addr); }
void hybrid_jmp_interpret(uint32_t addr) { jumped=addr; }
static void start(uint32_t base,const uint8_t *image) {
    memcpy(memory+base,image,0x200);memset(&g_cpu,0,sizeof g_cpu);
    active_base=base;invalid=g_redirect_pending=rte_pending=g_rte_resume=0;
    traps=absolute_calls=0;g_cpu.A[7]=0x7f00;
}
static void state(void) {
    CHECK(g_cpu.A[0]==active_base+0xa0);
    CHECK(g_cpu.D[0]==2 && g_cpu.D[1]==2 && g_cpu.D[2]==0xfedc);
    CHECK(g_cpu.D[3]==0x1234 && g_cpu.D[4]==0xfffffedc);
    CHECK(g_cpu.D[5]==1 && g_cpu.D[7]==7);
    CHECK(traps==1 && absolute_calls==1 && !g_redirect_pending);
    CHECK(g_cpu.A[7]==0x7f00);
}
int main(int argc,char **argv) {
    uint8_t image[0x200];if (argc!=2) return 1;
    FILE *file=fopen(argv[1],"rb");if (!file) return 1;
    int read=fread(image,1,sizeof image,file)==sizeof image;fclose(file);if (!read) return 1;
    CHECK(fixture_has_offset(0x52) && fixture_has_offset(0xb2) && fixture_has_offset(0x7e));
    CHECK(!fixture_has_offset(0x7c) && !fixture_has_offset(0x53));
    for (unsigned i=0;i<2;i++) {
        start(i?0x4000:0x2000,image);
        fixture_base=0x99;CHECK(fixture_dispatch(active_base,0x52));CHECK(fixture_base==0x99);
        state();CHECK(fixture_dispatch(active_base,0x7e));CHECK((uint16_t)g_cpu.D[6]==0xffff);
        CHECK(g_cpu.A[7]==0x7f00);
    }
    /* Abandon the C call chain in a split BSR callee, then resume at that
     * exact instruction and its relocated guest return (depth-zero model). */
    start(0x2000,image);interrupt_pc=active_base+0xb2;
    if (!setjmp(interrupt)) { fixture_dispatch(active_base,0x52);CHECK(0); }
    CHECK(g_cpu.A[7]==0x7efc && m68k_read32(g_cpu.A[7])==active_base+0x6a);
    CHECK(fixture_dispatch(active_base,0xb2));
    uint32_t ret=m68k_read32(g_cpu.A[7]);g_cpu.A[7]+=4;
    CHECK(fixture_dispatch(active_base,ret-active_base));state();
    invalid=1;g_cpu.D[6]=123;CHECK(fixture_dispatch(active_base,0x7e));
    CHECK(g_redirect_pending && g_redirect_addr==active_base+0x7e && g_cpu.D[6]==123);
    invalid=0;g_redirect_pending=0;
    CHECK(fixture_dispatch(active_base,0xc0));CHECK(jumped==active_base+0x100c8);
    CHECK(fixture_dispatch(active_base,0xd0));CHECK(jumped==active_base+0x110);
    return failures?1:0;
}
