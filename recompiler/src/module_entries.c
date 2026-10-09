#include "module_entries.h"

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
int cdi_module_entries(const uint8_t *data,uint32_t size,uint8_t type,
                       bool named_offset_pairs,uint32_t *entries,int capacity,
                       uint32_t *metadata_end) {
    if (!data || !entries || !metadata_end || capacity<1 || size<0x3b) return -1;
    uint32_t limit=size-3,entry=be32(data+0x30);
    if ((entry&1) || entry<0x38 || entry>=limit) return -1;
    if (type==1) {
        if (size<0x4b || entry<0x48) return -1;
        *metadata_end=0x48;entries[0]=entry;
        uint32_t exception=be32(data+0x34);
        if (!exception) return 1;
        if (capacity<2 || (exception&1) || exception<0x48 || exception>=limit) return -1;
        entries[1]=exception;return 2;
    }
    if (type!=2 || !named_offset_pairs) return -1;
    uint32_t pos=entry,end=entry;
    int count=0;
    for (;;) {
        if (pos>limit || limit-pos<8) return -1;
        uint32_t name=be32(data+pos),code=be32(data+pos+4);
        pos+=8;
        if (!name && !code) break;
        if (!name || !code || name<0x38 || name>=limit ||
            (code&1) || code<0x38 || code>=limit || count==capacity) return -1;
        uint32_t n=name;
        while (n<limit && n-name<255 && data[n]) {
            if (data[n]<0x20 || data[n]>0x7e) return -1;
            n++;
        }
        if (n==name || n==limit || n-name==255) return -1;
        if (n+1>end) end=n+1;
        entries[count++]=code;
    }
    if (!count) return -1;
    if (pos>end) end=pos;
    /* Metadata is never an executable discovery seed. */
    for (int i=0;i<count;i++) if (entries[i]<end) return -1;
    *metadata_end=end;return count;
}
