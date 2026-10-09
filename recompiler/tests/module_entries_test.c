#include "module_entries.h"
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);failures++; } } while (0)
static void word32(uint8_t *data,int at,uint32_t value) {
    data[at]=value>>24;data[at+1]=value>>16;data[at+2]=value>>8;data[at+3]=value;
}
int main(void) {
    uint8_t image[256]={0};uint32_t entries[8],end;
    word32(image,0x30,0x52);word32(image,0x34,0x90);
    CHECK(cdi_module_entries(image,256,1,false,entries,8,&end)==2);
    CHECK(entries[0]==0x52 && entries[1]==0x90 && end==0x48);
    word32(image,0x34,0);CHECK(cdi_module_entries(image,256,1,false,entries,8,&end)==1);
    word32(image,0x30,0x3c);CHECK(cdi_module_entries(image,256,1,false,entries,8,&end)==-1);
    word32(image,0x3c,0x54);word32(image,0x40,0x60);
    word32(image,0x44,0x5a);word32(image,0x48,0x80);
    memcpy(image+0x54,"first\0second\0",13);
    CHECK(cdi_module_entries(image,256,2,false,entries,8,&end)==-1);
    /* second name ends at $61; move code beyond all metadata. */
    word32(image,0x40,0x70);
    CHECK(cdi_module_entries(image,256,2,true,entries,8,&end)==2);
    CHECK(entries[0]==0x70 && entries[1]==0x80 && end==0x61);
    CHECK(cdi_module_entries(image,256,2,true,entries,1,&end)==-1);
    word32(image,0x48,0x81);CHECK(cdi_module_entries(image,256,2,true,entries,8,&end)==-1);
    word32(image,0x48,0x80);word32(image,0x44,254);
    CHECK(cdi_module_entries(image,256,2,true,entries,8,&end)==-1);
    word32(image,0x44,0x5a);memset(image+0x5a,'x',256-0x5a);
    CHECK(cdi_module_entries(image,256,2,true,entries,8,&end)==-1);
    return failures?1:0;
}
