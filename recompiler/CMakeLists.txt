cmake_minimum_required(VERSION 3.16)
project(CdiRecomp C)

set(CMAKE_C_STANDARD 11)

set(M68K_RECOMP_CORE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/m68k-recomp-core")
include("${M68K_RECOMP_CORE_DIR}/cmake/M68kRecompCore.cmake")
m68k_recomp_core_sources(M68K_RECOMP_CORE_SOURCES scc68070)
m68k_recomp_core_include_dirs(M68K_RECOMP_CORE_INCLUDE_DIRS scc68070)

set(CDI_FRONTEND_SOURCES
    src/rom_parser.c
    ${M68K_RECOMP_CORE_SOURCES}
    src/game_config.c
    src/toml.c
)

add_executable(CdiRecomp
    src/main_cdi.c
    src/disc_parser.c
    ${CDI_FRONTEND_SOURCES}
)
target_include_directories(CdiRecomp PRIVATE src ${M68K_RECOMP_CORE_INCLUDE_DIRS})

set_target_properties(CdiRecomp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
    target_compile_options(CdiRecomp PRIVATE /W3 /WX-)
endif()

# --- CdiRecompBios: BIOS-first entry (mirrors psxrecomp's psxrecomp-bios) ---
# Recompiles the CD-RTOS system ROM. Links the full 68000 frontend so --emit can
# drive function discovery + code generation over the ROM (based at $400000).
add_executable(CdiRecompBios
    src/main_cdi_bios.c
    src/disc_parser.c
    ${CDI_FRONTEND_SOURCES}
)
target_include_directories(CdiRecompBios PRIVATE src ${M68K_RECOMP_CORE_INCLUDE_DIRS})
set_target_properties(CdiRecompBios PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(MSVC)
    target_compile_options(CdiRecompBios PRIVATE /W3 /WX-)
endif()

# Generic flat-ROM frontend driver used only by synthetic code-discovery and
# code-generation regression fixtures. It deliberately reuses the preserved
# Genesis-origin CLI because those fixtures test the shared 68000 frontend,
# not CD-i disc loading. Excluded from normal builds and runtime artifacts.
add_executable(CdiRecompFrontendFixture EXCLUDE_FROM_ALL
    src/main_genesis.c
    ${CDI_FRONTEND_SOURCES}
)
target_include_directories(CdiRecompFrontendFixture PRIVATE src ${M68K_RECOMP_CORE_INCLUDE_DIRS})
set_target_properties(CdiRecompFrontendFixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(MSVC)
    target_compile_options(CdiRecompFrontendFixture PRIVATE /W3 /WX-)
endif()
