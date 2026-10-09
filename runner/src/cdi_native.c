/* Static module binding observes the real loader's RAM writes. It never
 * intercepts F$Load/F$Link or installs guest pointers. Exact full-image SHA256
 * matching admits an image; every overlapping write revokes it before the
 * next emitted instruction. Generated offset maps support IRQ/TRAP resumes. */
#include "cdi_native.h"
#include "cdi_runtime.h"
#include "cdi_sha256.h"
#include "debug_server.h"
#include <stdlib.h>
#include <string.h>

#ifndef CDI_NATIVE_MODULES
const CdiNativeModule g_cdi_native_modules[]={{0}};
const size_t g_cdi_native_module_count=0;
#endif

extern uint8_t g_ram0[CDI_RAM0_SIZE], g_ram1[CDI_RAM1_SIZE];
enum { WORDS_PER_BANK=CDI_RAM0_SIZE/2, TAG_CAPACITY=WORDS_PER_BANK*2, BINDING_CAPACITY=1024 };
/* Dense set of even RAM words containing the OS-9 sync, including candidates
 * still being copied. Removing a tag swaps the last item into its slot. */
static uint32_t tags[TAG_CAPACITY], tag_indices[TAG_CAPACITY], tag_count;
typedef struct {
    uint32_t base,size;
    uint64_t epoch;
    const CdiNativeModule *module;
    int checked,valid;
} Binding;
static Binding bindings[BINDING_CAPACITY];
static uint32_t binding_count;
static CdiNativeState stats;
static uint64_t epoch,event_count;
enum { EVENT_CAPACITY=1024 };
static CdiNativeEvent events[EVENT_CAPACITY];
/* Coverage is cumulative across streamed images, not a trace ring. A hash
 * index keeps repeated interpreter entries independent of the ledger length;
 * the dense records still provide stable pagination and first-seen provenance.
 * Half-full indexing bounds collision chains. Exhaustion remains explicit. */
enum { TARGET_CAPACITY=262144, TARGET_INDEX_CAPACITY=TARGET_CAPACITY*2 };
static CdiNativeTarget targets[TARGET_CAPACITY];
static uint32_t target_indices[TARGET_INDEX_CAPACITY];
static uint32_t target_count;
static uint64_t target_dropped;
static void event(const Binding *b,uint8_t type) {
    CdiNativeEvent *e=&events[event_count%EVENT_CAPACITY];
    *e=(CdiNativeEvent){.seq=event_count++,.trace_seq=debug_trace_sequence(),
        .frame=g_frame_count,.cycles=g_total_cycles,.epoch=b->epoch,.pc=g_cpu.PC,
        .base=b->base,.size=b->size,.module=b->module?(uint32_t)(b->module-g_cdi_native_modules):UINT32_MAX,.type=type};
}

static uint8_t *ram(uint32_t address, uint32_t size) {
    if (address<CDI_RAM0_SIZE && size<=CDI_RAM0_SIZE-address) return g_ram0+address;
    if (address>=CDI_RAM1_BASE && address-CDI_RAM1_BASE<CDI_RAM1_SIZE &&
        size<=CDI_RAM1_SIZE-(address-CDI_RAM1_BASE)) return g_ram1+address-CDI_RAM1_BASE;
    return NULL;
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint32_t tag_index(uint32_t address) {
    return address<CDI_RAM0_SIZE ? address/2 : WORDS_PER_BANK+(address-CDI_RAM1_BASE)/2;
}
void cdi_native_reset(void) {
    memset(tag_indices,0,sizeof tag_indices);tag_count=0;
    memset(bindings,0,sizeof bindings);binding_count=0;
    memset(&stats,0,sizeof stats);stats.compiled=(uint32_t)g_cdi_native_module_count;
    /* Tokens remain unique across warm resets with retained guest RAM. */
    event_count=0;
    target_count=0;target_dropped=0;
    memset(target_indices,0,sizeof target_indices);
    /* Resetting CPU bookkeeping need not clear RAM (e.g. a warm reset). */
    cdi_native_notify_write(CDI_RAM0_BASE,CDI_RAM0_SIZE);
    cdi_native_notify_write(CDI_RAM1_BASE,CDI_RAM1_SIZE);
}
void cdi_native_notify_write(uint32_t address, uint32_t size) {
    if (!g_cdi_native_module_count || !size || !ram(address,size)) return;
    uint64_t end=(uint64_t)address+size;
    for (uint32_t i=0;i<binding_count;i++) {
        Binding *b=&bindings[i];
        if (address<(uint64_t)b->base+b->size && end>b->base) {
            if (b->valid) { stats.invalidations++;stats.active--;event(b,CDI_NATIVE_INVALIDATE); }
            b->valid=0;b->checked=0;
        }
    }
    for (uint32_t a=address&~1u;(uint64_t)a<end;a+=2) {
        uint8_t *p=ram(a,2);
        if (!p) continue;
        uint32_t index=tag_index(a), slot=tag_indices[index];
        if (p[0]==0x4a && p[1]==0xfc) {
            if (!slot) { tags[tag_count]=a;tag_indices[index]=++tag_count; }
        } else if (slot) {
            uint32_t last=tags[--tag_count];
            tags[slot-1]=last;tag_indices[tag_index(last)]=slot;tag_indices[index]=0;
        }
    }
}
static Binding *cached(uint32_t base) {
    for (uint32_t i=0;i<binding_count;i++) if (bindings[i].base==base) return &bindings[i];
    return NULL;
}
static Binding *resolve(uint32_t address) {
    for (uint32_t i=0;i<binding_count;i++) {
        Binding *b=&bindings[i];
        if (b->valid && address>=b->base && address-b->base<b->size) return b;
    }
    if (!ram(address,2)) return NULL;
    for (uint32_t i=0;i<tag_count;i++) {
        uint32_t base=tags[i];
        if (base>address) continue;
        uint8_t *p=ram(base,0x30);
        if (!p) continue;
        uint32_t size=be32(p+4);
        if (size<0x33 || address-base>=size || !ram(base,size)) continue;
        Binding *b=cached(base);
        if (b && b->checked) continue; /* unchanged identity rejection */
        int possible=0;
        for (size_t m=0;m<g_cdi_native_module_count;m++)
            if (g_cdi_native_modules[m].size==size) { possible=1;break; }
        if (!possible) continue;
        uint16_t parity=0;
        for (unsigned w=0;w<0x30;w+=2) parity^=(uint16_t)((p[w]<<8)|p[w+1]);
        if (parity!=0xffff) continue;
        if (!b) {
            if (binding_count==BINDING_CAPACITY) {
                debug_dump_fault_trail("native module binding capacity exceeded");abort();
            }
            b=&bindings[binding_count++];b->base=base;
        }
        b->size=size;b->checked=1;b->valid=0;b->module=NULL;
        uint8_t digest[32];cdi_sha256(p,size,digest);
        for (size_t m=0;m<g_cdi_native_module_count;m++) {
            const CdiNativeModule *module=&g_cdi_native_modules[m];
            if (module->size==size && !memcmp(module->sha256,digest,32)) {
                b->module=module;b->valid=1;b->epoch=++epoch;stats.bindings++;stats.active++;
                event(b,CDI_NATIVE_BIND);
                return b;
            }
        }
        stats.identity_rejections++;
        event(b,CDI_NATIVE_REJECT);
    }
    return NULL;
}
int cdi_native_has_address(uint32_t address) {
    if (!g_cdi_native_module_count || (address&1)) return 0;
    Binding *b=resolve(address);
    return b && b->module->has_offset(address-b->base);
}
int cdi_native_dispatch(uint32_t address) {
    if (!g_cdi_native_module_count || (address&1)) return 0;
    Binding *b=resolve(address);
    if (!b || !b->module->has_offset(address-b->base)) return 0;
    stats.dispatches++;
    return b->module->dispatch(b->base,address-b->base);
}
uint64_t cdi_native_execution_token(uint32_t base,CdiNativeDispatch expected) {
    for (uint32_t i=0;i<binding_count;i++) {
        const Binding *b=&bindings[i];
        if (b->valid && b->base==base && b->module->dispatch==expected)
            return (b->epoch<<16)|(i+1);
    }
    return 0;
}
int cdi_native_instruction_valid(uint32_t address,CdiNativeDispatch expected,uint64_t token) {
    uint32_t slot=(uint32_t)(token&0xffff);
    if (!slot || slot>binding_count) return 0;
    const Binding *b=&bindings[slot-1];
    return b->valid && b->epoch==(token>>16) && b->module->dispatch==expected &&
           !(address&1) && address>=b->base && address-b->base<b->size;
}
void cdi_native_state(CdiNativeState *out) { *out=stats; }
int cdi_native_events(CdiNativeEvent *out,int capacity,uint64_t from,uint64_t *total,uint64_t *oldest) {
    uint64_t end=event_count,first=end>EVENT_CAPACITY?end-EVENT_CAPACITY:0;
    if (total) *total=end;if (oldest) *oldest=first;
    if (capacity<=0) return 0;
    if (from==UINT64_MAX) from=end>(uint64_t)capacity?end-capacity:0;
    if (from<first) from=first;
    int count=0;
    while (from<end && count<capacity) out[count++]=events[from++%EVENT_CAPACITY];
    return count;
}
void cdi_native_record_target(uint32_t address) {
    if (!g_cdi_native_module_count || (address&1)) return;
    Binding *b=resolve(address);
    if (!b) return;
    uint32_t module=(uint32_t)(b->module-g_cdi_native_modules),offset=address-b->base;
    if (b->module->has_offset(offset)) return;
    uint32_t hash=module*0x9e3779b9u ^ (offset>>1)*0x85ebca6bu;
    hash^=hash>>16;
    uint32_t slot=hash&(TARGET_INDEX_CAPACITY-1);
    while (target_indices[slot]) {
        CdiNativeTarget *t=&targets[target_indices[slot]-1];
        if (t->module==module && t->offset==offset) { t->hits++;return; }
        slot=(slot+1)&(TARGET_INDEX_CAPACITY-1);
    }
    if (target_count==TARGET_CAPACITY) { target_dropped++;return; }
    target_indices[slot]=target_count+1;
    targets[target_count++]=(CdiNativeTarget){.module=module,.offset=offset,.base=b->base,
        .epoch=b->epoch,.frame=g_frame_count,.trace_seq=debug_trace_sequence(),.hits=1};
}
int cdi_native_targets(CdiNativeTarget *out,int capacity,uint32_t from,uint32_t *total,uint64_t *dropped) {
    if (total) *total=target_count;if (dropped) *dropped=target_dropped;
    int count=0;
    while (from<target_count && count<capacity) out[count++]=targets[from++];
    return count;
}
