#include "cdi_diagnostics.h"
#include "cdi_runtime.h"
#include "cdi_input.h"
#include "cdi_mod.h"
#include "player_config.h"
#include "recomp_diagnostics/replay_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static RecompReplayWriter *writer;
static int hook_registered,hook_installed;
static const char *hook_id="cdi.diagnostics.input-recording";
static void finish(void) {
    if(hook_installed){uint8_t identity[32];if(!cdi_mod_set_builtin_hook(hook_id,0,identity))fprintf(stderr,"[recording] hook removal failed\n");hook_installed=0;}
    if(writer){recomp_replay_end(writer);const char *error=recomp_replay_destroy(writer);writer=NULL;if(error)fprintf(stderr,"[recording] %s\n",error);}
}
void cdi_diagnostics_begin(int enabled,const char *profile_path) {
    if(!hook_registered){if(!cdi_mod_register_hook(hook_id,cdi_diagnostics_frame)){fprintf(stderr,"[recording] trusted hook registration failed\n");return;}hook_registered=1;}
    uint8_t identity[32];
    if(!enabled){if(!cdi_mod_set_builtin_hook(hook_id,0,identity))fprintf(stderr,"[recording] disabled hook plan rejected\n");return;}
    char path[1200];
    if(!profile_path || !*profile_path)return;
    if(!cdi_player_config_sibling_path(profile_path,"diagnostics-input.log",path,sizeof path))return;
    writer=recomp_replay_create_profile(RECOMP_REPLAY_HASHED64,32u*1024u*1024u);
    if(!writer || !recomp_replay_begin_files(writer,path,"CDI INPUT 1\n",profile_path,"","native SCC68070; original image read-only\n")) {
        fprintf(stderr,"[recording] cannot open input recording\n");finish();return;
    }
    if(!cdi_mod_set_builtin_hook(hook_id,1,identity)){fprintf(stderr,"[recording] hook plan rejected\n");finish();return;}
    hook_installed=1;
    char plan_line[68]="P ";for(unsigned i=0;i<32;i++)sprintf(plan_line+2+i*2,"%02x",identity[i]);strcat(plan_line,"\n");
    recomp_replay_line(writer,plan_line);
    atexit(finish);
}
void cdi_diagnostics_frame(void) {
    if(!writer)return;
    char line[128];int dx,dy;cdi_input_pending_relative(&dx,&dy);
    snprintf(line,sizeof line,"I %llu %u %d %d\n",(unsigned long long)g_frame_count,cdi_input_get(),dx,dy);
    if(!recomp_replay_line(writer,line)){fprintf(stderr,"[recording] input segment stopped: %s\n",recomp_replay_error(writer));finish();}
}
