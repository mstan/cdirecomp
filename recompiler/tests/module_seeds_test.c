#include "module_seeds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1); } } while (0)
static const char *path="module-seeds-test.txt";
static void write_seed(const char *text) {
    FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs(text,f)>=0);CHECK(!fclose(f));
}
int main(void) {
    const char *digest="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char text[1024];CdiModuleSeeds seeds;
    snprintf(text,sizeof text,"# captured callbacks\n%s 0x40\n%s 40 # duplicate\n%s 80\n",digest,digest,digest);
    write_seed(text);CHECK(cdi_module_seeds_read(path,&seeds));CHECK(seeds.count==3);
    uint8_t other[32]={0};uint32_t entries[4]={0x38};int count=1;
    CHECK(cdi_module_seeds_merge(&seeds,other,0x38,0x100,entries,4,&count));CHECK(count==1);CHECK(!seeds.items[0].matched);
    CHECK(cdi_module_seeds_merge(&seeds,seeds.items[0].sha256,0x38,0x100,entries,4,&count));
    CHECK(count==3 && entries[1]==0x40 && entries[2]==0x80 && seeds.items[2].matched);
    count=1;CHECK(!cdi_module_seeds_merge(&seeds,seeds.items[0].sha256,0x42,0x100,entries,4,&count));
    count=1;CHECK(!cdi_module_seeds_merge(&seeds,seeds.items[0].sha256,0x38,0x80,entries,4,&count));
    count=1;CHECK(!cdi_module_seeds_merge(&seeds,seeds.items[0].sha256,0x38,0x100,entries,1,&count));
    cdi_module_seeds_free(&seeds);
    const char *bad[]={"41","100000000","0x","40garbage","40 extra"};
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) {
        snprintf(text,sizeof text,"%s %s\n",digest,bad[i]);write_seed(text);
        CHECK(!cdi_module_seeds_read(path,&seeds));CHECK(!seeds.items && !seeds.count);
    }
    write_seed("01234 40\n");CHECK(!cdi_module_seeds_read(path,&seeds));
    snprintf(text,sizeof text,"g%s 40\n",digest+1);write_seed(text);CHECK(!cdi_module_seeds_read(path,&seeds));
    snprintf(text,sizeof text,"%s40\n",digest);write_seed(text);CHECK(!cdi_module_seeds_read(path,&seeds));
    CHECK(!remove(path));return 0;
}
