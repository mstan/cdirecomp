/* Independent polynomial arithmetic versus the native fold, supplemented by
 * exact exceptional-state materialization. The actual recompiled firmware is
 * compared separately by the loader runtime fixture and built-game harness. */
#include "../src/cdi_loader_math.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
uint8_t g_rom[CDI_ROM_SIZE];
static uint32_t seed=0x9C68CD10u;
static uint32_t random32(void) {seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static uint32_t bitwise(uint32_t crc,uint32_t word) {
    crc&=0xFFFFFFu;
    for(int byte=3;byte>=0;byte--) {
        crc^=((word>>(byte*8))&255u)<<16;
        for(int bit=0;bit<8;bit++) {crc<<=1;if(crc&0x1000000u)crc^=0x800063u;}
        crc&=0xFFFFFFu;
    }
    return crc;
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    FILE *rom=fopen(argv[1],"rb");if(!rom)return 2;
    size_t bytes=fread(g_rom,1,sizeof g_rom,rom);fclose(rom);
    if(bytes<0x6C68u)return 2;
    for(unsigned i=0;i<100000;i++) {
        M68KState before={0};
        for(unsigned r=0;r<8;r++){before.D[r]=random32();before.A[r]=random32();}
        before.D[5]=0x800021u;before.SR=(uint16_t)random32();
        uint32_t word=random32();int odd;
        M68KState folded=fold_word(&before,word,&odd);
        CrcFlight flight={before,word,odd?30u:29u,0x406C38u,1};
        M68KState exact=prefix(&flight);
        int differs=folded.PC!=exact.PC||folded.SR!=exact.SR||folded.USP!=exact.USP||folded.SSP!=exact.SSP;
        for(unsigned r=0;r<8;r++)differs|=folded.D[r]!=exact.D[r]||folded.A[r]!=exact.A[r];
        if(differs) {
            for(unsigned r=0;r<8;r++)if(folded.D[r]!=exact.D[r])fprintf(stderr,"D%u %08X/%08X\n",r,folded.D[r],exact.D[r]);
            fprintf(stderr,"loader materialization mismatch case%u seed%08X, crc%08X/%08X SR%04X/%04X\n",i,seed,folded.D[1],exact.D[1],folded.SR,exact.SR);return 1;
        }
        if((folded.D[1]&0xFFFFFFu)!=bitwise(before.D[1],word)) {
            fprintf(stderr,"loader polynomial mismatch case%u: %06X/%06X\n",i,folded.D[1]&0xFFFFFFu,bitwise(before.D[1],word));return 1;
        }
    }
    puts("loader fold:100000 independent polynomial/register/flag comparisons PASS");return 0;
}
