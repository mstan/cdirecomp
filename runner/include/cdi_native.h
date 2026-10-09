#pragma once
#include <stddef.h>
#include <stdint.h>

typedef int (*CdiNativeDispatch)(uint32_t base, uint32_t offset);
typedef struct {
    const char *name;
    uint32_t size;
    uint8_t sha256[32];
    int (*has_offset)(uint32_t);
    CdiNativeDispatch dispatch;
} CdiNativeModule;

extern const CdiNativeModule g_cdi_native_modules[];
extern const size_t g_cdi_native_module_count;

void cdi_native_reset(void);
/* Called AFTER every CPU/DMA RAM write. No guest loader state is replaced. */
void cdi_native_notify_write(uint32_t address, uint32_t size);
int cdi_native_has_address(uint32_t address);
int cdi_native_dispatch(uint32_t address);
/* An instruction safepoint rejects stale code after writes/unload/overlay. */
uint64_t cdi_native_execution_token(uint32_t base, CdiNativeDispatch expected);
int cdi_native_instruction_valid(uint32_t address, CdiNativeDispatch expected, uint64_t token);

typedef struct {
    uint64_t bindings, invalidations, dispatches, identity_rejections;
    uint32_t active, compiled;
} CdiNativeState;
void cdi_native_state(CdiNativeState *out);

enum { CDI_NATIVE_BIND=1, CDI_NATIVE_INVALIDATE=2, CDI_NATIVE_REJECT=3 };
typedef struct {
    uint64_t seq,trace_seq,frame,cycles,epoch;
    uint32_t pc,base,size,module;
    uint8_t type;
} CdiNativeEvent;
int cdi_native_events(CdiNativeEvent *out,int capacity,uint64_t from,
                       uint64_t *total,uint64_t *oldest);
/* Uncovered entries retain image identity at execution time, so a reused RAM
 * address can never promote the previous overlay's offset into another image. */
typedef struct {
    uint32_t module,offset,base;
    uint64_t epoch,frame,trace_seq,hits;
} CdiNativeTarget;
void cdi_native_record_target(uint32_t address);
int cdi_native_targets(CdiNativeTarget *out,int capacity,uint32_t from,
                        uint32_t *total,uint64_t *dropped);
