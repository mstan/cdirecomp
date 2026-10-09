/* Synthetic PIC executable, independent of the copyrighted disc and BIOS. */
#include "code_generator.h"
#include "codegen_diag.h"
#include "function_finder.h"
#include <stdio.h>
#include <string.h>

static void word(uint8_t *p,unsigned at,unsigned value) { p[at]=value>>8;p[at+1]=value; }
int main(int argc,char **argv) {
    bool negative=argc==3 && !strcmp(argv[2],"--test-falloff");
    if (argc!=2 && !negative) return 1;
    uint8_t image[0x200]={0};
    const unsigned words[]={
        0x41fa,0x004c, /* lea $a0(pc),a0 */
        0x303a,0x0048, /* move.w $a0(pc),d0 */
        0x7202,        /* moveq #2,d1 */
        0x343b,0x1042, /* move.w $a0(pc,d1.w),d2 */
        0x4cba,0x0018,0x003c, /* movem.w $a0(pc),d3-d4 */
        0x6100,0x0048, /* bsr $b0 */
        0x203c,0x0000,0x0002, /* move.l #2,d0 */
        0x4ebb,0x083e, /* jsr $b0(pc,d0.l), target $b2 */
        0x4eb9,0x0000,0x00b8, /* absolute jsr $b8 must remain absolute */
        0x4e40,0x0001, /* OS-9 inline selector is data, post-PC is $7e */
        0x7c01,0x51ce,0xfffe, /* moveq #1,d6; dbf d6,$80 */
        0x4e75
    };
    for (unsigned i=0;i<sizeof words/sizeof words[0];i++) word(image,0x52+i*2,words[i]);
    word(image,0xa0,0x1234);word(image,0xa2,0xfedc);
    word(image,0xb0,0x5245);word(image,0xb2,0x4e75);
    word(image,0xb8,0x7e63);word(image,0xba,0x4e75);
    word(image,0xc0,0x207c);word(image,0xc2,1);word(image,0xc4,0);
    word(image,0xc6,0x4efb);word(image,0xc8,0x8800); /* JMP (PC,A0.L), +$10000 */
    word(image,0xd0,0x207c);word(image,0xd2,1);word(image,0xd4,0x38);
    word(image,0xd6,0x4efb);word(image,0xd8,0x8000); /* JMP (PC,A0.W), +0 */
    word(image,0x110,0x4e75);
    GenesisRom rom={0};rom.rom_data=image;rom.rom_size=sizeof image;rom.initial_pc=0x52;
    uint32_t entries[]={0x52,0xc0,0xd0,0x1fe};
    if (negative) word(image,0x1fe,0x4e71); /* NOP with no legal successor */
    GameConfig cfg={0};game_config_init_empty(&cfg);
    cfg.function_aliases=true;cfg.async_resume_entries=true;cfg.trap0_inline_service_word=true;
    cfg.extra_funcs=entries;cfg.extra_func_count=negative?4:3;
    ProtectedRange header={0,0x52};cfg.protected_ranges=&header;cfg.protected_range_count=1;
    FunctionList functions={0};function_finder_run_module(&rom,&functions,&cfg);
    AnnotationTable annotations={0};
    char full[1200],dispatch[1200],raw[1200];
    snprintf(full,sizeof full,"%s/fixture%s_full.c",argv[1],negative?"_bad":"");
    snprintf(dispatch,sizeof dispatch,"%s/fixture%s_dispatch.c",argv[1],negative?"_bad":"");
    snprintf(raw,sizeof raw,"%s/fixture%s.bin",argv[1],negative?"_bad":"");
    FILE *file=fopen(raw,"wb");if (!file) return 1;
    int written=fwrite(image,1,sizeof image,file)==sizeof image;fclose(file);
    int ok=written && codegen_emit_module(&rom,&functions,full,dispatch,&annotations,&cfg,"fixture");
    function_list_free(&functions);
    return written && (negative?!ok:ok) && !codegen_diag_total()?0:1;
}
