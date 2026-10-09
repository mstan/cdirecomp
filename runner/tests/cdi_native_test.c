/* Synthetic identities: exercise loader observation independently of codegen. */
#include "cdi_native.h"
#include "cdi_runtime.h"
#include <stdio.h>
#include <string.h>

uint8_t g_ram0[CDI_RAM0_SIZE],g_ram1[CDI_RAM1_SIZE];
M68KState g_cpu;
uint64_t g_frame_count,g_total_cycles;
uint64_t debug_trace_sequence(void) { return 0; }
void debug_dump_fault_trail(const char *reason) { fprintf(stderr,"%s\n",reason); }
static int failures, calls;
static uint32_t last_base,last_offset;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);failures++; } } while (0)
static int has_offset(uint32_t offset) { return offset==52 || offset==54; }
static int dispatch_a(uint32_t base,uint32_t offset) { calls++;last_base=base;last_offset=offset;return 1; }
static int dispatch_b(uint32_t base,uint32_t offset) { return dispatch_a(base,offset); }
const CdiNativeModule g_cdi_native_modules[]={
    {"test1",64,{0x2e,0xba,0x2a,0x05,0x14,0xd8,0x9c,0x08,0xd0,0x4d,0x6f,0xb3,0xd3,0xfa,0xa3,0xcc,
                0x3c,0xd1,0xb5,0xa5,0x0b,0xd4,0x8b,0x2c,0x45,0x30,0x2d,0x6b,0x24,0xdd,0xc9,0x35},has_offset,dispatch_a},
    {"test2",64,{0x6c,0x83,0x6a,0x43,0xc7,0xad,0xc6,0x15,0xeb,0x76,0x50,0x06,0x18,0x75,0x0b,0x4c,
                0xc1,0x22,0x7a,0x7b,0xd5,0x3c,0xe6,0xab,0x63,0x76,0x03,0xd1,0xb3,0x6b,0x58,0x5a},has_offset,dispatch_b},
    {"large",65536,{0x9a,0x25,0xe8,0x4c,0x3e,0x15,0xbe,0x3c,0x6d,0x9a,0xfa,0xf3,0x0d,0x4c,0x79,0xa0,
                    0x7c,0xcd,0x6a,0xa0,0x45,0x36,0x8b,0xc1,0x97,0xb0,0x27,0xf7,0xb6,0x23,0xa0,0xaa},has_offset,dispatch_a}
};
const size_t g_cdi_native_module_count=3;
static const uint8_t image[]={
    0x4a,0xfc,0,0,0,0,0,0x40,0,0,0,0,0,0,0,0x38,
    0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0xb4,0x7a,
    0,0,0,0x34,0x70,1,0x4e,0x75,0x74,0x65,0x73,0x74,0xb1,0xaa,0xbb,1
};
static uint8_t *pointer(uint32_t address) {
    return address<CDI_RAM0_SIZE ? g_ram0+address : g_ram1+address-CDI_RAM1_BASE;
}
static void copy(uint32_t base,const uint8_t *data,uint32_t size) {
    memcpy(pointer(base),data,size);cdi_native_notify_write(base,size);
}
static void byte(uint32_t address,uint8_t value) {
    *pointer(address)=value;cdi_native_notify_write(address,1);
}
int main(void) {
    const uint32_t a=0x2000,b=0x210000;
    CdiNativeState state;uint64_t token_a=0,token_b=0;
    cdi_native_reset();
    CHECK(!cdi_native_has_address(a+52));
    copy(a,image,60); /* incomplete loader copy must not bind */
    CHECK(!cdi_native_has_address(a+52));
    copy(a+60,image+60,4);
    CHECK(cdi_native_has_address(a+52));
    token_a=cdi_native_execution_token(a,dispatch_a);
    CHECK(!cdi_native_has_address(a+53));CHECK(!cdi_native_has_address(a+56));
    cdi_native_record_target(a+56);cdi_native_record_target(a+56);
    CdiNativeTarget target[4];uint32_t total;uint64_t dropped;
    CHECK(cdi_native_targets(target,4,0,&total,&dropped)==1);
    CHECK(total==1 && !dropped && target[0].module==0 && target[0].offset==56 && target[0].hits==2);
    CHECK(cdi_native_dispatch(a+54));CHECK(calls==1 && last_base==a && last_offset==54);
    CHECK(cdi_native_instruction_valid(a+52,dispatch_a,token_a));
    CHECK(!cdi_native_instruction_valid(a+52,dispatch_b,token_a));
    copy(b,image,64);
    CHECK(cdi_native_has_address(b+52));CHECK(cdi_native_dispatch(b+52));
    CHECK(calls==2 && last_base==b && last_offset==52);
    token_b=cdi_native_execution_token(b,dispatch_a);
    byte(a+51,image[51]); /* even unchanged overlapping write revokes execution */
    CHECK(!cdi_native_instruction_valid(a+52,dispatch_a,token_a));
    CHECK(cdi_native_instruction_valid(b+52,dispatch_a,token_b));
    CHECK(cdi_native_has_address(a+52));
    CHECK(!cdi_native_instruction_valid(a+52,dispatch_a,token_a));
    token_a=cdi_native_execution_token(a,dispatch_a);
    byte(a+52,image[52]^1);
    CHECK(!cdi_native_instruction_valid(a+54,dispatch_a,token_a));
    CHECK(!cdi_native_has_address(a+52));CHECK(!cdi_native_dispatch(a+52));
    byte(a+52,image[52]);CHECK(cdi_native_has_address(a+54));
    /* Same-size overlay at the same address must reject the old C frame. */
    byte(a+60,0xb2);byte(a+63,2);
    CHECK(!cdi_native_instruction_valid(a+52,dispatch_a,token_a));
    CHECK(cdi_native_has_address(a+52));
    CHECK(!cdi_native_instruction_valid(a+52,dispatch_a,token_a));
    token_a=cdi_native_execution_token(a,dispatch_b);
    CHECK(cdi_native_instruction_valid(a+52,dispatch_b,token_a));
    cdi_native_record_target(a+56);
    CHECK(cdi_native_targets(target,4,0,&total,&dropped)==2);
    CHECK(target[1].module==1 && target[1].offset==56 && target[1].epoch!=target[0].epoch);
    byte(a,0);CHECK(!cdi_native_has_address(a+52));
    byte(a,0x4a);CHECK(cdi_native_has_address(a+52));
    /* Tag removal must retain the other bank's swapped dense-set entry. */
    CHECK(cdi_native_has_address(b+52));
    byte(b+64,0x4a);CHECK(cdi_native_instruction_valid(b+52,dispatch_a,token_b));
    copy(0x7ffe0,image,32); /* truncated at the end of RAM cannot bind */
    CHECK(!cdi_native_has_address(0x7fffc));CHECK(!cdi_native_has_address(0x80000));
    CHECK(!cdi_native_has_address(0x400034));
    cdi_native_state(&state);
    CHECK(state.compiled==3 && state.active==2 && state.invalidations>=4);
    CHECK(state.identity_rejections>=2 && state.dispatches==2);
    cdi_native_reset(); /* warm-reset RAM is rediscovered without loader writes */
    CHECK(!cdi_native_instruction_valid(a+52,dispatch_b,token_a));
    CHECK(cdi_native_has_address(a+52));CHECK(cdi_native_has_address(b+52));
    token_a=cdi_native_execution_token(a,dispatch_b);
    CHECK(cdi_native_instruction_valid(a+52,dispatch_b,token_a));
    /* Sustained coverage must exceed the former 16384-entry limit. Copies at
     * different bases and fresh epochs aggregate by image/offset, while the
     * paged first-seen records remain stable. */
    static uint8_t large[65536];
    memcpy(large,image,sizeof image);
    large[4]=0;large[5]=1;large[6]=0;large[7]=0;
    large[46]=0xb4;large[47]=0x3b;
    copy(0x230000,large,sizeof large);
    copy(0x240000,large,sizeof large);
    cdi_native_reset();
    for (uint32_t offset=0;offset<40004;offset+=2)
        cdi_native_record_target(0x230000+offset);
    for (uint32_t offset=0;offset<40004;offset+=2)
        cdi_native_record_target(0x240000+offset);
    byte(0x230000+51,large[51]);
    cdi_native_record_target(0x230000+40002);
    CHECK(cdi_native_targets(target,1,19999,&total,&dropped)==1);
    CHECK(total==20000 && dropped==0 && target[0].module==2 &&
          target[0].offset==40002 && target[0].hits==3 && target[0].base==0x230000);
    CHECK(cdi_native_targets(target,1,20000,&total,&dropped)==0);
    cdi_native_reset();
    CHECK(cdi_native_targets(target,4,0,&total,&dropped)==0 && total==0 && dropped==0);
    cdi_native_record_target(0x230000+40002);
    CHECK(cdi_native_targets(target,4,0,&total,&dropped)==1 && target[0].hits==1);
    return failures?1:0;
}
