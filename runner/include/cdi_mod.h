#pragma once
#include "recomp_mod.h"
/* The caller owns a resolved, identity-bound plan. Apply only on the machine
 * owner thread at an instruction/field boundary, with execution paused. */
int cdi_mod_apply(rmod_context *,const rmod_patch *,size_t,const rmod_hook *,size_t,uint8_t plan_identity[32]);
/* Trusted host callbacks; a committed frame hook executes on the owner thread. */
int cdi_mod_register_hook(const char *id,void (*callback)(void));
void cdi_mod_frame(void);
