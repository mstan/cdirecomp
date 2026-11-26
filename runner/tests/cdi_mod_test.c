#include "cdi_mod.h"
#include "cdi_runtime.h"
#include <string.h>
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed at %d: %s\n",__LINE__,#x);return 1;}}while(0)
uint8_t g_ram0[CDI_RAM0_SIZE],g_ram1[CDI_RAM1_SIZE],g_rom[CDI_ROM_SIZE],image[64];
static int calls,invalidations;
void cdi_native_notify_write(uint32_t a,uint32_t n){(void)a;(void)n;invalidations++;}
int cdi_media_mod_read(uint64_t a,void *b,size_t n){if(a>sizeof image || n>sizeof image-a)return 0;memcpy(b,image+a,n);return 1;}
int cdi_media_mod_write(uint64_t a,const void *b,size_t n){if(a>sizeof image || n>sizeof image-a)return 0;memcpy(image+a,b,n);return 1;}
static void callback(void){calls++;}
int cdi_media_original_sha256(char out[65]){strcpy(out,"1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");return 1;}
int main(void) {
    const char *sha="1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";
    const char *plan="abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
    rmod_context *ctx=rmod_create();CHECK(ctx);CHECK(rmod_bind_plan(ctx,"cdi.fixture",sha,plan));
    uint8_t before=0,after=23,identity[32];
    rmod_patch patches[2]={{"ram",CDI_RAM0_BASE+32,&before,&after,1},{"image",3,&before,&after,1}};
    rmod_hook bad={"missing"},good={"fixture.frame"};
    CHECK(cdi_mod_register_hook(good.id,callback));
    CHECK(rmod_bind_plan(ctx,"wrong",sha,plan));
    CHECK(!cdi_mod_apply(ctx,patches,2,&good,1,identity));
    CHECK(rmod_bind_plan(ctx,"cdi.fixture",sha,plan));
    CHECK(!cdi_mod_apply(ctx,patches,2,&bad,1,identity));
    CHECK(g_ram0[32]==0 && image[3]==0);cdi_mod_frame();CHECK(!calls);
    CHECK(cdi_mod_apply(ctx,patches,2,&good,1,identity));
    CHECK(g_ram0[32]==23 && image[3]==23 && invalidations>=2);cdi_mod_frame();CHECK(calls==1);
    CHECK(!cdi_mod_apply(ctx,patches,2,NULL,0,identity));
    CHECK(g_ram0[32]==23 && image[3]==23);
    rmod_destroy(ctx);puts("CD-i guarded RAM/image writes, identity, expected bytes and hook rollback passed");return 0;
}
