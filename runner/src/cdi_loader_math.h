#pragma once
#include "cdi_runtime.h"
extern uint8_t g_rom[CDI_ROM_SIZE];
typedef struct {
    M68KState before;
    uint32_t word;
    unsigned completed;
    uint32_t pc;
    int active;
} CrcFlight;
static void logical(M68KState *s, uint32_t v, unsigned bits) {
    uint32_t mask = bits == 32 ? UINT32_MAX : (1u << bits) - 1u;
    s->SR &= (uint16_t)~0x0Fu;
    if (!(v & mask)) s->SR |= SR_Z;
    if (v & (1u << (bits - 1))) s->SR |= SR_N;
}
static uint32_t shift(M68KState *s, uint32_t v, unsigned bits,
                      unsigned count, int left) {
    uint32_t mask = bits == 32 ? UINT32_MAX : (1u << bits) - 1u;
    uint32_t carry = left ? ((v >> (bits - count)) & 1u) : ((v >> (count - 1)) & 1u);
    uint32_t result = (left ? v << count : (v & mask) >> count) & mask;
    s->SR &= (uint16_t)~0x1Fu;
    logical(s, result, bits);
    if (carry) s->SR |= SR_X | SR_C;
    return result;
}

/* Exceptional state reconstruction up to a device/IRQ frontier. This recipe
 * is not the production checksum algorithm and does no reads or clock work. */
static M68KState prefix(const CrcFlight *f) {
    M68KState s = f->before;
    for (unsigned i = 0; i < f->completed; i++) {
        switch (i) {
        case 0: s.D[1]=shift(&s,s.D[1],32,8,1);break;
        case 1: s.D[0]=f->word;s.A[0]+=4;logical(&s,s.D[0],32);break;
        case 2: s.D[1]^=s.D[0];logical(&s,s.D[1],32);break;
        case 3: s.D[2]=s.D[1];logical(&s,s.D[2],32);break;
        case 4: s.D[4]=(s.D[4]&0xFFFF0000u)|(s.D[1]&0xFFFFu);logical(&s,s.D[4],16);break;
        case 5: s.D[2]&=0xFFFF0000u;s.SR=(s.SR&~0x0Fu)|SR_Z;break;
        case 6: s.D[2]=(s.D[2]>>16)|(s.D[2]<<16);logical(&s,s.D[2],32);break;
        case 7: s.D[0]=0xFFFFFF83u;logical(&s,s.D[0],32);break;
        case 8: s.D[0]=(s.D[0]&0xFFFF0000u)|((s.D[0]&s.D[2])&0xFFFFu);logical(&s,s.D[0],16);break;
        case 9: s.D[4]^=s.D[0]&0xFFFFu;logical(&s,s.D[4],16);break;
        case 10:s.D[0]=0x7FFFFF;logical(&s,s.D[0],32);break;
        case 11:s.D[0]&=s.D[1];logical(&s,s.D[0],32);break;
        case 12:s.D[0]=shift(&s,s.D[0],32,5,1);break;
        case 13:s.D[1]^=s.D[0];logical(&s,s.D[1],32);break;
        case 14:s.D[0]=s.D[1];logical(&s,s.D[0],32);break;
        case 15:s.D[2]=(s.D[2]&0xFFFF0000u)|(s.D[2]&0xFFFCu);logical(&s,s.D[2],16);break;
        case 16:s.D[2]=shift(&s,s.D[2],32,3,1);break;
        case 17:s.D[1]^=s.D[2];logical(&s,s.D[1],32);break;
        case 18:s.D[2]=23;logical(&s,s.D[2],32);break;
        case 19:s.D[0]=shift(&s,s.D[0],32,23,0);break;
        case 20:s.D[1]^=s.D[0];logical(&s,s.D[1],32);break;
        case 21:{uint32_t v=s.D[1],r=v+v;s.D[1]=r;s.SR&=(uint16_t)~0x1Fu;logical(&s,r,32);if(v>>31)s.SR|=SR_X|SR_C;if(((v^r)&0x80000000u))s.SR|=SR_V;break;}
        case 22:s.D[0]=(s.D[0]&0xFFFF0000u)|(s.D[4]&0xFFFFu);logical(&s,s.D[0],16);break;
        case 23:s.D[0]=(s.D[0]&0xFFFF0000u)|shift(&s,s.D[0],16,8,0);break;
        case 24:s.D[0]^=s.D[4]&0xFFu;logical(&s,s.D[0],8);break;
        case 25:s.D[4]=(s.D[4]&0xFFFF0000u)|(s.D[0]&0xFFFFu);logical(&s,s.D[4],16);break;
        case 26:s.D[4]=(s.D[4]&0xFFFFFF00u)|shift(&s,s.D[4],8,3,0);break;
        case 27:{unsigned bit=(g_rom[0x6BC0+(s.D[4]&0xFFFFu)]>>(s.D[0]&7))&1;s.SR=(s.SR&~SR_Z)|(bit?0:SR_Z);break;}
        case 28:break;
        case 29:s.D[1]^=s.D[5];logical(&s,s.D[1],32);break;
        }
    }
    s.PC=f->pc;
    return s;
}

static M68KState fold_word(const M68KState *in, uint32_t word, int *odd) {
    M68KState out=*in;
    uint32_t t=(in->D[1]<<8)^word;
    uint32_t parity_word=(t&0xFFFFu)^((t>>16)&0xFF83u);
    uint32_t u=t^((t&0x7FFFFFu)<<5);
    uint32_t crc=(u^(((t>>16)&0xFFFCu)<<3)^(u>>23))*2u;
    unsigned p=((parity_word>>8)^parity_word)&255u;
    *odd=(g_rom[0x6BC0+(p>>3)]>>(p&7))&1;
    if (*odd) crc^=in->D[5];
    out.A[0]+=4;out.D[0]=p;out.D[1]=crc;out.D[2]=23;
    out.D[4]=(in->D[4]&0xFFFF0000u)|(p>>3);
    out.SR=(in->SR&~0x1Fu)|((p&4)?SR_X:0);
    if (*odd) logical(&out,crc,32);
    else out.SR|=SR_Z|((p&4)?SR_C:0);
    out.PC=0x406C38u;
    return out;
}

