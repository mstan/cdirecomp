#include "driver_callbacks.h"
#include "m68k_decoder.h"
#include "m68k_validator.h"
#include <string.h>

int cdi_driver_callbacks(const GenesisRom *rom, const char *name,
                         unsigned type, unsigned lang,
                         uint32_t begin, uint32_t size,
                         uint32_t *entries, int capacity) {
    /* ciapdriv owns the publication and use of this private vector. Other
     * drivers may assign entirely different meanings to A2-relative data. */
    if (!rom || !name || strcmp(name,"ciapdriv") || type!=0x0e || lang!=1 ||
        !entries || capacity<0 || (begin&1u) || size<0x50u ||
        begin>rom->rom_size || size>rom->rom_size-begin) return 0;
    const uint32_t end=begin+size-3u; /* OS-9 CRC is metadata. */
    const uint32_t code_begin=begin+0x48u;
    const uint16_t slots[]={0x008c,0x0090,0x0094};
    unsigned called=0;
    for(uint32_t pc=code_begin;pc+6u<=end;pc+=2u) {
        /* MOVEA.L slot(A2),A0; JSR(A0): same-module consuming ABI. */
        if(rom_read16(rom,pc)!=0x206a || rom_read16(rom,pc+4u)!=0x4e90)continue;
        for(unsigned i=0;i<3;i++)if(rom_read16(rom,pc+2u)==slots[i])called|=1u<<i;
    }
    M68KValidatorOptions options={0};
    int count=0;
    for(uint32_t pc=code_begin;pc+8u<=end;pc+=2u) {
        /* LEA handler(PC),A0; MOVE.L A0,slot(A2). This also covers
         * publications inside callbacks, closing the whole vector family. */
        if(rom_read16(rom,pc)!=0x41fa || rom_read16(rom,pc+4u)!=0x2548)continue;
        unsigned slot=3;
        for(unsigned i=0;i<3;i++)if(rom_read16(rom,pc+6u)==slots[i])slot=i;
        if(slot==3 || !(called&(1u<<slot)))continue;
        uint32_t target=pc+2u+(uint32_t)(int32_t)(int16_t)rom_read16(rom,pc+2u);
        M68KInstr first;
        if((target&1u) || target<code_begin || target>=end ||
            !m68k_decode(rom,target,&first) || target+first.byte_length>end ||
            m68k_validate(&first,&options)!=M68K_LEGAL)continue;
        int duplicate=0;
        for(int i=0;i<count;i++)if(entries[i]==target)duplicate=1;
        if(duplicate)continue;
        if(count==capacity)return -1;
        entries[count++]=target;
    }
    return count;
}
