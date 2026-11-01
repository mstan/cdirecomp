#pragma once
#include <stdint.h>

typedef int (*CdiBoardPump)(void);
typedef struct {uint64_t jobs,quiet_quanta,faithful_quanta,observation_exits;} CdiBoardJobState;
uint32_t cdi_board_idle_quanta(uint32_t maximum,CdiBoardPump pump,int *quit);
void cdi_board_job_state(CdiBoardJobState *out);
/* Stopped-CPU only: each limit is STRICTLY before an observable event.
 * Advance helpers require a checked quiet prefix with no intervening guest
 * MMIO/mutation. The original 25-cycle quantum handles the critical event. */
unsigned periph_quiet_quanta(unsigned maximum);
void periph_advance_quiet(uint32_t cycles);
unsigned slave_quiet_quanta(unsigned maximum,uint64_t quantum_ns);
void slave_advance_quiet(uint64_t ns);
unsigned cdi_streaming_quiet_quanta(unsigned maximum,uint64_t quantum_ns);
unsigned nvram_quiet_quanta(unsigned maximum,double quantum_ns);
void nvram_advance_quiet(unsigned quanta,double quantum_ns);
unsigned runtime_pacing_quiet_quanta(unsigned maximum);
int debug_board_job_allowed(void);
