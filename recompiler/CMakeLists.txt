cmake_minimum_required(VERSION 3.16)
project(CdiRecomp C)

set(CMAKE_C_STANDARD 11)

set(CDI_FRONTEND_SOURCES
    src/rom_parser.c
    src/m68k_decoder.c
    src/m68k_validator.c
    src/function_finder.c
    src/code_generator.c
    src/codegen_diag.c
    src/annotations.c
    src/game_config.c
    src/toml.c
)

add_executable(CdiRecomp
    src/main_cdi.c
    src/disc_parser.c
    ${CDI_FRONTEND_SOURCES}
)
target_include_directories(CdiRecomp PRIVATE src)

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
target_include_directories(CdiRecompBios PRIVATE src)
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
target_include_directories(CdiRecompFrontendFixture PRIVATE src)
set_target_properties(CdiRecompFrontendFixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(MSVC)
    target_compile_options(CdiRecompFrontendFixture PRIVATE /W3 /WX-)
endif()
