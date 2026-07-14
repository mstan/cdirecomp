cmake_minimum_required(VERSION 3.16)
project(CdiRecomp C)

set(CMAKE_C_STANDARD 11)

# The shared 68000 frontend was copied verbatim from segagenesisrecomp
# (see PROVENANCE.txt). cycle_probe.c links the clown68000 interpreter so it can
# single-step each instruction at codegen time and emit exact cycle counts. We
# vendor clown68000 under external/ (the same source the Genesis core uses);
# CLOWN68000_COVERAGE_LOG_DISABLE keeps codegen traffic out of its coverage table.
#
# NOTE: SCC68070 timing differs from the base 68000 (and CD-i adds wait states).
# clown68000 cycle counts are a correct-instruction-set starting point; 68070
# timing refinement is tracked in TODO.md (MC-CDI-005).
set(C68K_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/clown68000/source")

add_executable(CdiRecomp
    src/main_cdi.c
    src/disc_parser.c
    src/rom_parser.c
    src/m68k_decoder.c
    src/m68k_validator.c
    src/function_finder.c
    src/code_generator.c
    src/codegen_diag.c
    src/annotations.c
    src/game_config.c
    src/cycle_probe.c
    src/toml.c
    "${C68K_DIR}/interpreter/clown68000.c"
    "${C68K_DIR}/common/opcode.c"
)

target_compile_definitions(CdiRecomp PRIVATE
    CLOWN68000_COVERAGE_LOG_DISABLE
)

target_include_directories(CdiRecomp PRIVATE
    src
    "${C68K_DIR}/interpreter"
    "${C68K_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/clowncommon"
)

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
    src/rom_parser.c
    src/m68k_decoder.c
    src/m68k_validator.c
    src/function_finder.c
    src/code_generator.c
    src/codegen_diag.c
    src/annotations.c
    src/game_config.c
    src/cycle_probe.c
    src/toml.c
    "${C68K_DIR}/interpreter/clown68000.c"
    "${C68K_DIR}/common/opcode.c"
)
target_compile_definitions(CdiRecompBios PRIVATE CLOWN68000_COVERAGE_LOG_DISABLE)
target_include_directories(CdiRecompBios PRIVATE
    src
    "${C68K_DIR}/interpreter"
    "${C68K_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/clowncommon"
)
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
    src/rom_parser.c
    src/m68k_decoder.c
    src/m68k_validator.c
    src/function_finder.c
    src/code_generator.c
    src/codegen_diag.c
    src/annotations.c
    src/game_config.c
    src/cycle_probe.c
    src/toml.c
    "${C68K_DIR}/interpreter/clown68000.c"
    "${C68K_DIR}/common/opcode.c"
)
target_compile_definitions(CdiRecompFrontendFixture PRIVATE
    CLOWN68000_COVERAGE_LOG_DISABLE
)
target_include_directories(CdiRecompFrontendFixture PRIVATE
    src
    "${C68K_DIR}/interpreter"
    "${C68K_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/clowncommon"
)
set_target_properties(CdiRecompFrontendFixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(MSVC)
    target_compile_options(CdiRecompFrontendFixture PRIVATE /W3 /WX-)
endif()
