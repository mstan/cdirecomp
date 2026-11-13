# Architecture

## Compilation and execution

`CdiRecompBios` loads a user-supplied CD-RTOS system ROM, discovers entry points and emits native C plus a byte-identity header. `CdiRecomp` inventories a raw Mode-2 disc, validates OS-9 modules and emits position-independent native module bodies and a source manifest.

The shared disc/module parser and byte-image adapter live in `common/`; production players do not compile compiler drivers or the flat-cartridge test parser.

The SCC68070 frontend comes from the pinned clean-room `m68k-recomp-core` submodule. The engine supplies CD-i image/configuration adapters and disc/module handling. Generated C includes the runtime ABI in `runner/include/cdi_runtime.h`; synthetic fixtures compile against that same contract.

The native player executes the real recompiled OS-9 kernel. The kernel owns loading, linking, scheduling and guest state. Loaded module bindings use complete image SHA-256 identities, with invalidation on overlapping RAM writes. Uncovered code uses the clean-room fallback interpreter.

## Machine and host layers

The runtime models the SCC68070 address space, interrupts/peripherals, MCD212 video timing and scanout, MCD221 CIAP media/XA audio, IKAT input and DS1216 clock/NVRAM. `cdi_frontend.c` presents completed frames and routes physical input; it does not synthesize guest results.

Complete native jobs provide qualified alternatives for scanout, loader verification, drawing preparation, memory operations, selected services, streaming, stopped-board progression and raster presentation. Each retains a separately selectable LLE implementation. Backend identity is a build setting; correctness and performance acceptance are per workload and per game.

## Repository boundaries

The engine owns generic compiler/runtime behavior and component checks. A game repository pins the engine and shared launcher, owns title facts and generation outputs, and builds its own named player target through `cmake/CdiGame.cmake`. Neither repository distributes firmware or disc bytes.

Production runtime packages use explicit dependency/asset allowlists. Optional oracle development code is isolated and excluded. See [provenance](../PROVENANCE.md), [game setup](GAME_PROJECT_SETUP.md), and [validation](VALIDATION.md).
