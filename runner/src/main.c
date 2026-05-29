/*
 * main.c — CdiRuntime entry point (skeleton).
 *
 * The runtime links the generated C (hotelmario/generated/*.c) against the
 * device + OS-9 HLE layer and drives execution from the boot module's entry.
 * That last step needs the CD-RTOS loader (TODO MC-CDI-001); until it exists,
 * this boots the runtime substrate, arms the debug ring, and reports honestly
 * that there is nothing to execute yet — no silent fake "running" state.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cdi_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* runtime-internal debug surface (debug_server.c) */
void debug_server_init(int port);
void debug_server_poll(void);
void debug_ring_capture_frame(void);

int main(int argc, char *argv[]) {
    const char *disc = NULL;
    int port = 4380;   /* native; oracle (CeDImu) on +1 — see TCP.md */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (argv[i][0] != '-') disc = argv[i];
    }

    printf("CdiRuntime — Philips CD-i (SCC68070) static-recomp runtime\n");
    if (disc) printf("  disc: %s\n", disc);

    runtime_init();
    debug_server_init(port);

    fprintf(stderr,
        "\n[cdi] Runtime substrate is up (memory map + device routing + OS-9\n"
        "      trap gateway), but no recompiled code is linked and the CD-RTOS\n"
        "      boot loader is not implemented yet.\n"
        "      Next milestones (TODO.md):\n"
        "        MC-CDI-001  CD-RTOS / OS-9 module loader (relocate cdi_hotel)\n"
        "        MC-CDI-016  oracle bring-up against CeDImu\n");
    return 0;
}
