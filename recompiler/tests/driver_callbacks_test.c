#include "driver_callbacks.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static unsigned char image[0x400];
static void word(unsigned a,unsigned v){image[a]=(unsigned char)(v>>8);image[a+1]=(unsigned char)v;}
static void publish(unsigned pc,unsigned target,unsigned slot){word(pc,0x41fa);word(pc+2,target-pc-2);word(pc+4,0x2548);word(pc+6,slot);}
static void consume(unsigned pc,unsigned slot){word(pc,0x206a);word(pc+2,slot);word(pc+4,0x4e90);}
int main(void){
    memset(image,0xff,sizeof image);GenesisRom rom={0};rom.rom_data=image;rom.rom_size=sizeof image;uint32_t found[8];
    for(unsigned i=0;i<3;i++){publish(0x150+i*8,0x1a0+i*8,0x8c+i*4);consume(0x180+i*6,0x8c+i*4);word(0x1a0+i*8,0x4e75);}
    /* Backward signed offset and a nested publication are genuine callbacks. */
    publish(0x1a8,0x1a0,0x8c);publish(0x1b0,0x1c0,0x90);word(0x1c0,0x4e75);
    CHECK(cdi_driver_callbacks(&rom,"ciapdriv",14,1,0x100,0x100,found,8)==4);
    CHECK(found[0]==0x1a0 && found[1]==0x1a8 && found[2]==0x1b0 && found[3]==0x1c0);
    CHECK(cdi_driver_callbacks(&rom,"ciapdriv",14,1,0x100,0x100,found,3)==-1);
    CHECK(cdi_driver_callbacks(&rom,"other",14,1,0x100,0x100,found,8)==0);
    CHECK(cdi_driver_callbacks(&rom,"ciapdriv",13,1,0x100,0x100,found,8)==0);
    CHECK(cdi_driver_callbacks(&rom,"ciapdriv",14,0,0x100,0x100,found,8)==0);
    word(0x180,0x206b); /* Different state-owner register does not prove a call. */
    CHECK(cdi_driver_callbacks(&rom,"ciapdriv",14,1,0x100,0x100,found,8)==3);
    publish(0x158,0x202,0x90); /* Outside module. */
    publish(0x160,0x1b1,0x94); /* Odd target. */
    publish(0x1b0,0x1c0,0x90);word(0x1c0,0x1040); /* Illegal MOVEA.B. */
    CHECK(cdi_driver_callbacks(&rom,"ciapdriv",14,1,0x100,0x100,found,8)==0);
    puts("CIAP callback vector publication, consumption, closure and guards passed");return 0;
}
