# Building cdirecomp

Use CMake 3.20+, a C11 compiler, Ninja, Python 3.11+ and SDL2 development files. Initialize submodules recursively before configuring. On Windows, use an actual MinGW or Visual Studio toolchain; on Linux install the SDL2 development package.

The root CMake project builds the compilers and component tests without private inputs. `BUILD_TESTING=OFF` omits tests. The recompiler also builds independently with `cmake -S recompiler -B build/recompiler`.

## Generate a system ROM

Create the output parent directory, then run:

```sh
build/recompiler/CdiRecompBios /path/to/player.rom --emit --out build/generated/bios --seeds bios/cdrtos_discovered.txt
```

Use `.exe` on Windows. `--out` is required for emission. The output contains `cdrtos_full.c`, `cdrtos_dispatch.c` and `bios_identity.h`. The player rejects BIOS bytes that differ from this identity. The seeds are extra observed entry points, not replacement routines.

## Build the standalone player

```sh
cmake -S . -B build/player -G Ninja -DCMAKE_BUILD_TYPE=Release -DCDI_BUILD_RUNTIME=ON -DCDI_BIOS_GENERATED_DIR=/absolute/path/to/generated/bios
cmake --build build/player --parallel 2
```

This builds `runner/CdiRuntime` (`.exe` on Windows). `CDI_GAME_GENERATED_DIR` optionally supplies native OS-9 modules; an empty value builds a system player with interpreter fallback. It never defaults to a particular game's directory.

## Generate disc modules

```sh
build/recompiler/CdiRecomp /path/to/game.cue --emit --out /path/to/game-project/generated/modules
```

Export-layout options and observed module seeds are game facts. The named-offset32 Subr convention must be selected explicitly by a title that uses it. Do not assume it applies to every OS-9 module.

## Reference and native implementations

Each `CDI_*_BACKEND` selects `LLE` or `HLE` at build time: VIDEO, LOADER, DRAWING, MEMORY, SERVICE, STREAMING, BOARD and GPU. Engine defaults are LLE. Consumers choose qualified defaults. `CDI_COSIM_BUILD=OFF` excludes co-simulation instrumentation from release builds.

Build artifacts and generated data are disposable. Native saves and settings are not: do not delete an old build directory without checking its contents.
