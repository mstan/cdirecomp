# Separate game projects

A game repository pins `cdirecomp/` and, when used, `recomp-ui/` as root submodules. Recursive Git checkout is the source identity; do not replace it with a text pin or require an adjacent developer checkout.

The game owns `game.toml`, its observed module-entry catalog, launcher adapter, product assets, build scripts and acceptance checks. Its private input paths are ignored. Generated BIOS and modules live under the game's ignored `generated/bios/` and `generated/modules/`, or an explicit equivalent outside the engine.

After generation, use the engine's consumer entry point:

```cmake
include("${CMAKE_CURRENT_SOURCE_DIR}/cdirecomp/cmake/CdiGame.cmake")
cdirecomp_add_game(MyGame NAME "My Game"
    BIOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/bios"
    MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/modules")
```

This creates the named player target and applies the disc requirement and product name. It adds the engine runtime without building the compiler into the distributed product. Add title-specific launcher code and shared UI to that target, then finalize its build provenance.

Use [HotelMarioRecomp](https://github.com/mstan/HotelMarioRecomp) as the complete consumer example. Its cross-platform Python build driver verifies the supplied image identities, builds the pinned compilers, generates both inputs locally and builds the product. The Windows and shell wrappers call that same driver.

Keep generic device and compiler defects in the engine. Game-specific layouts, title acceptance, branding and release policy belong in the consumer. Never commit user ROMs, discs, battery images, settings or generated guest code.
