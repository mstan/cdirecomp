#include "cdi_diagnostics.h"
#include "cdi_runtime.h"
#include "cdi_input.h"
#include "player_config.h"
#include "recomp_diagnostics/replay_c.h"
#include <stdio.h>
#include <stdlib.h>
static RecompReplayWriter *writer;
static void finish(void) {if(writer){recomp_replay_end(writer);const char *error=recomp_replay_destroy(writer);writer=NULL;if(error)fprintf(stderr,"[recording] %s\n",error);}}
void cdi_diagnostics_begin(int enabled,const char *profile_path) {
    if(!enabled)return;
    char path[1200];
    if(!profile_path || !*profile_path)return;
    if(!cdi_player_config_sibling_path(profile_path,"diagnostics-input.log",path,sizeof path))return;
    writer=recomp_replay_create();
    if(!writer || !recomp_replay_begin_files(writer,path,"CDI INPUT 1\n",profile_path,"","native SCC68070; original image read-only\n")) {
        fprintf(stderr,"[recording] cannot open input recording\n");finish();return;
    }
    atexit(finish);
}
void cdi_diagnostics_frame(void) {
    if(!writer)return;
    char line[128];int dx,dy;cdi_input_pending_relative(&dx,&dy);
    snprintf(line,sizeof line,"I %llu %u %d %d\n",(unsigned long long)g_frame_count,cdi_input_get(),dx,dy);
    if(!recomp_replay_line(writer,line)){fprintf(stderr,"[recording] input segment stopped: %s\n",recomp_replay_error(writer));finish();}
}
