#include "module_seeds.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_digit(unsigned char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}

void cdi_module_seeds_free(CdiModuleSeeds *seeds) {
    free(seeds->items);
    memset(seeds,0,sizeof *seeds);
}

bool cdi_module_seeds_read(const char *path, CdiModuleSeeds *out) {
    enum { MAX_SEEDS=32768 };
    memset(out,0,sizeof *out);
    if (!path) return true;
    FILE *file=fopen(path,"r");
    if (!file) return false;
    out->items=calloc(MAX_SEEDS,sizeof *out->items);
    bool ok=out->items!=NULL;
    char line[160];
    unsigned line_number=0;
    while (ok && fgets(line,sizeof line,file)) {
        line_number++;
        /* Reject long lines even when their valid prefix would fit. */
        if (!strchr(line,'\n') && !feof(file)) { ok=false;break; }
        char *p=line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p=='#') continue;
        if (out->count==MAX_SEEDS || strlen(p)<66) { ok=false;break; }
        CdiModuleSeed seed={0};
        for (unsigned i=0;i<32;i++) {
            int hi=hex_digit((unsigned char)p[i*2]),lo=hex_digit((unsigned char)p[i*2+1]);
            if (hi<0 || lo<0) { ok=false;break; }
            seed.sha256[i]=(uint8_t)((hi<<4)|lo);
        }
        if (!ok) break;
        p+=64;
        if (!isspace((unsigned char)*p)) { ok=false;break; }
        while (isspace((unsigned char)*p)) p++;
        if (p[0]=='0' && (p[1]=='x' || p[1]=='X')) p+=2;
        unsigned digits=0;
        int digit;
        while ((digit=hex_digit((unsigned char)*p))>=0) {
            if (++digits>8) { ok=false;break; }
            seed.offset=(seed.offset<<4)|(unsigned)digit;
            p++;
        }
        if (!ok || !digits || (seed.offset&1)) { ok=false;break; }
        while (isspace((unsigned char)*p)) p++;
        if (*p && *p!='#') { ok=false;break; }
        out->items[out->count++]=seed;
    }
    if (ferror(file)) ok=false;
    if (fclose(file)!=0) ok=false;
    if (!ok) {
        fprintf(stderr,"[modules] invalid seed file %s at line %u\n",path,line_number);
        cdi_module_seeds_free(out);
    }
    return ok;
}

bool cdi_module_seeds_merge(CdiModuleSeeds *seeds,const uint8_t digest[32],
                            uint32_t metadata_end,uint32_t code_end,
                            uint32_t *entries,size_t capacity,int *count) {
    if (*count<0 || (size_t)*count>capacity) return false;
    for (size_t i=0;i<seeds->count;i++) {
        CdiModuleSeed *seed=&seeds->items[i];
        if (memcmp(seed->sha256,digest,32)) continue;
        seed->matched=true;
        if (seed->offset<metadata_end || seed->offset>=code_end || (seed->offset&1))
            return false;
        bool present=false;
        for (int j=0;j<*count;j++) if (entries[j]==seed->offset) { present=true;break; }
        if (present) continue;
        if ((size_t)*count==capacity) return false;
        entries[(*count)++]=seed->offset;
    }
    return true;
}
