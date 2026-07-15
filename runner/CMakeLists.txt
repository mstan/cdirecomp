cmake_minimum_required(VERSION 3.16)
project(CdiRuntime C)

set(CMAKE_C_STANDARD 11)

# MC-CDI-016: differential co-simulation instrument (RAM/CPU state hashing,
# fault injection, cosim_full_ram_hash). See docs/COSIM-SPEC.md. Default ON —
# cosim_state.c is gated behind CDI_COSIM internally and compiles to an empty
# TU when this is OFF, so it is always safe to list in the source set below.
option(CDI_COSIM_BUILD "Build the MC-CDI-016 differential co-simulation instrument (RAM/CPU state hashing + fault injection)" ON)

# Generated C is dropped in by the recompiler and compiled in here. It
# #includes cdi_runtime.h + game_extras.h from runner/include.
#   - bios/generated/*.c     : the recompiled CD-RTOS system ROM (Phase 1, boots).
#   - hotelmario/generated/*.c : the recompiled game (Phase 3, empty until then).
# OS-first: the BIOS is what we boot to the player shell before any game work.
file(GLOB BIOS_GEN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../bios/generated/*.c")
file(GLOB GAME_GEN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../hotelmario/generated/*.c")

# GCC's -O3 expansion of the ~450k-line generated ROM can exhaust the MinGW
# assembler even on otherwise roomy developer machines. -O2 keeps the runtime
# optimized while making clean Release builds deterministic and materially
# smaller at the compiler/assembler boundary.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/../bios/generated/cdrtos_full.c"
        PROPERTIES COMPILE_OPTIONS "-O2"
    )
endif()

add_executable(CdiRuntime
    src/main.c
    src/cdi_bus.c
    src/runtime.c
    src/mcd212.c
    src/mcd212_video.c
    src/cdi_frontend.c
    src/player_config.c
    src/cdi_input.c
    src/cdi_host_time.c
    src/cdi_media.c
    src/cdi_audio.c
    src/cdic.c
    src/slave.c
    src/periph.c
    src/cdi_nvram.c
    src/debug_server.c
    src/cosim_state.c
    src/m68k_arith.c
    src/m68k_interp.c
    # Clean-room hybrid interpreter (MC-CDI-011) reuses the recompiler's own
    # decoder for parity-by-construction with the static C emitter.
    "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src/m68k_decoder.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src/rom_parser.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src/disc_parser.c"
    ${BIOS_GEN_SRCS}
    ${GAME_GEN_SRCS}
)

target_include_directories(CdiRuntime PRIVATE
    include
    "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../bios/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/../hotelmario/generated"
)

set_target_properties(CdiRuntime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
    target_compile_options(CdiRuntime PRIVATE /W3 /WX-)
endif()

target_compile_definitions(CdiRuntime PRIVATE $<$<BOOL:${CDI_COSIM_BUILD}>:CDI_COSIM=1>)

find_package(SDL2 CONFIG REQUIRED)
target_link_libraries(CdiRuntime PRIVATE SDL2::SDL2)
if(WIN32)
    add_custom_command(TARGET CdiRuntime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:CdiRuntime>
        COMMENT "Copying SDL2 runtime beside CdiRuntime")
endif()

# Debug server (debug_server.c): winsock on Windows, pthreads elsewhere.
if(WIN32)
    target_link_libraries(CdiRuntime PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(CdiRuntime PRIVATE Threads::Threads)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(CdiNvramTest
        tests/cdi_nvram_test.c
        src/cdi_nvram.c
        src/cdi_host_time.c
    )
    target_include_directories(CdiNvramTest PRIVATE include)
    add_test(NAME cdi_nvram COMMAND CdiNvramTest)

    add_executable(PeriphTest
        tests/periph_test.c
        src/periph.c
    )
    target_include_directories(PeriphTest PRIVATE include)
    add_test(NAME scc68070_peripherals COMMAND PeriphTest)

    add_executable(Mcd212VideoTest
        tests/mcd212_video_test.c
        src/mcd212_video.c
    )
    target_include_directories(Mcd212VideoTest PRIVATE include)
    add_test(NAME mcd212_video COMMAND Mcd212VideoTest)

    add_executable(M68kArithTest
        tests/m68k_arith_test.c
        src/m68k_arith.c
    )
    target_include_directories(M68kArithTest PRIVATE include)
    add_test(NAME m68k_arithmetic COMMAND M68kArithTest)

    add_executable(CdiInputTest
        tests/cdi_input_test.c
        src/cdi_input.c
    )
    target_include_directories(CdiInputTest PRIVATE include)
    add_test(NAME cdi_input COMMAND CdiInputTest)

    add_executable(CdiAudioTest
        tests/cdi_audio_test.c
        src/cdi_audio.c
    )
    target_include_directories(CdiAudioTest PRIVATE include)
    add_test(NAME cdi_audio COMMAND CdiAudioTest)

    add_executable(CdicTest
        tests/cdic_test.c
        src/cdic.c
    )
    target_include_directories(CdicTest PRIVATE include)
    add_test(NAME cdic COMMAND CdicTest)

    add_executable(PlayerConfigTest
        tests/player_config_test.c
        src/player_config.c
    )
    target_include_directories(PlayerConfigTest PRIVATE include)
    target_link_libraries(PlayerConfigTest PRIVATE SDL2::SDL2)
    add_test(NAME player_config COMMAND PlayerConfigTest)
endif()
