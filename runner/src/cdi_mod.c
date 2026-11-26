#include "cdi_mod.h"
#include "cdi_runtime.h"
#include "cdi_native.h"
#include "cdi_media.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
typedef struct {char id[96];void (*callback)(void);int enabled,saved;} Hook;
static Hook hooks[32];static unsigned hook_count;static int applying;
#ifndef CDI_MOD_GAME_ID
#define CDI_MOD_GAME_ID "cdi.system"
#endif
extern uint8_t g_ram0[],g_ram1[],g_rom[];
static uint8_t *region(const char *space,uint64_t address,size_t size) {
    if(!strcmp(space,"ram")) {
        if(address>=CDI_RAM0_BASE && address-CDI_RAM0_BASE<=CDI_RAM0_SIZE && size<=CDI_RAM0_SIZE-(address-CDI_RAM0_BASE))return g_ram0+(address-CDI_RAM0_BASE);
        if(address>=CDI_RAM1_BASE && address-CDI_RAM1_BASE<=CDI_RAM1_SIZE && size<=CDI_RAM1_SIZE-(address-CDI_RAM1_BASE))return g_ram1+(address-CDI_RAM1_BASE);
    }
    if(!strcmp(space,"bios") && address<=CDI_ROM_SIZE && size<=CDI_ROM_SIZE-address)return g_rom+address;
    return NULL;
}
static int begin(void *user) {(void)user;if(applying)return 0;applying=1;for(unsigned i=0;i<hook_count;i++)hooks[i].saved=hooks[i].enabled;return 1;}
static int read_bytes(void *user,const char *space,uint64_t address,uint8_t *bytes,size_t size) {
    (void)user;uint8_t *p=region(space,address,size);if(p){memcpy(bytes,p,size);return 1;}
    if(!strcmp(space,"image") || !strcmp(space,"cartridge"))return cdi_media_mod_read(address,bytes,size);
    return 0;
}
static int write_bytes(void *user,const char *space,uint64_t address,const uint8_t *bytes,size_t size) {
    (void)user;uint8_t *p=region(space,address,size);if(p){memcpy(p,bytes,size);if(!strcmp(space,"ram"))cdi_native_notify_write((uint32_t)address,(uint32_t)size);return 1;}
    if(!strcmp(space,"image") || !strcmp(space,"cartridge"))return cdi_media_mod_write(address,bytes,size);
    return 0;
}
static int hook(void *user,const char *id) {(void)user;for(unsigned i=0;i<hook_count;i++)if(!strcmp(id,hooks[i].id)){hooks[i].enabled=1;return 1;}return 0;}
static void finish(void *user,int committed) {(void)user;if(!committed)for(unsigned i=0;i<hook_count;i++)hooks[i].enabled=hooks[i].saved;applying=0;}
int cdi_mod_apply(rmod_context *ctx,const rmod_patch *patches,size_t count,const rmod_hook *plan_hooks,size_t hooks_count,uint8_t identity[32]) {
    char sha[65];if(!cdi_media_original_sha256(sha))return 0;
    rmod_engine engine={NULL,CDI_MOD_GAME_ID,sha,begin,read_bytes,write_bytes,hook,finish};
    int result=rmod_apply(ctx,patches,count,plan_hooks,hooks_count,&engine,identity);
    if(result<0){fprintf(stderr,"[mods] transaction rollback failed; execution stopped\n");abort();}
    return result;
}
int cdi_mod_register_hook(const char *id,void (*callback)(void)) {
    if(!id || !*id || strlen(id)>=sizeof hooks[0].id || !callback || hook_count==32 || applying)return 0;
    for(unsigned i=0;i<hook_count;i++)if(!strcmp(id,hooks[i].id))return 0;
    strcpy(hooks[hook_count].id,id);hooks[hook_count++].callback=callback;return 1;
}
void cdi_mod_frame(void) {if(!applying)for(unsigned i=0;i<hook_count;i++)if(hooks[i].enabled)hooks[i].callback();}
