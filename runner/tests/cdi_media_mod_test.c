#include "cdi_media.h"
#include "disc_parser.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed at %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
    const char *path="cdi-mod-image-fixture.bin";
    uint8_t raw[CDI_RAW_SECTOR_SIZE]={0},body[2340],data[2048];
    FILE *f=fopen(path,"wb");CHECK(f);
    raw[15]=2;
    for(unsigned i=0;i<17;i++){memset(raw+24,0,2048);if(i==16)memcpy(raw+25,"CD-I ",5);CHECK(fwrite(raw,1,sizeof raw,f)==sizeof raw);}
    CHECK(!fclose(f));cdi_media_init();CHECK(cdi_media_mount(path));
    char hash_before[65],hash_after[65];CHECK(cdi_media_original_sha256(hash_before));
    uint8_t value=42,read=0;CHECK(cdi_media_mod_write(27,&value,1));
    CHECK(cdi_media_mod_read(27,&read,1) && read==42);
    CHECK(cdi_media_read_sector_body(0,body) && body[15]==42);
    CHECK(cdi_media_read_sector_form1(0,data) && data[3]==42);
    CHECK(cdi_media_original_sha256(hash_after) && !strcmp(hash_before,hash_after));
    f=fopen(path,"rb");CHECK(f);CHECK(fseek(f,27,SEEK_SET)==0 && fgetc(f)==0);fclose(f);
    CHECK(!cdi_media_mod_write(17u*CDI_RAW_SECTOR_SIZE,&value,1));
    cdi_media_eject();CHECK(!cdi_media_mod_read(27,&read,1));CHECK(!remove(path));
    puts("CD-i raw-image overlay reaches both sector consumers; original image unchanged");return 0;
}
