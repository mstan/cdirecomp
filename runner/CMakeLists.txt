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
set(CDI_GAME_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../hotelmario/generated" CACHE PATH "Directory of generated native OS-9 modules")
file(GLOB GAME_GEN_SRCS "${CDI_GAME_GENERATED_DIR}/*.c")
if(EXISTS "${CDI_GAME_GENERATED_DIR}/module_manifest.c")
    if(NOT EXISTS "${CDI_GAME_GENERATED_DIR}/module_sources.cmake")
        message(FATAL_ERROR "Regenerate native modules: module_sources.cmake is missing")
    endif()
    include("${CDI_GAME_GENERATED_DIR}/module_sources.cmake")
    set(GAME_GEN_SRCS "")
    foreach(MODULE_SOURCE IN LISTS CDI_NATIVE_MODULE_FILES)
        list(APPEND GAME_GEN_SRCS "${CDI_GAME_GENERATED_DIR}/${MODULE_SOURCE}")
    endforeach()
endif()

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
    src/cdi_native.c
    ../common/cdi_sha256.c
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
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/m68k-recomp-core/common/m68k_decoder.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src/rom_parser.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src/disc_parser.c"
    ${BIOS_GEN_SRCS}
    ${GAME_GEN_SRCS}
)

target_include_directories(CdiRuntime PRIVATE
    include
    ../common
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/m68k-recomp-core/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../bios/generated"
    "${CDI_GAME_GENERATED_DIR}"
)

set_target_properties(CdiRuntime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
    target_compile_options(CdiRuntime PRIVATE /W3 /WX-)
endif()

target_compile_definitions(CdiRuntime PRIVATE $<$<BOOL:${CDI_COSIM_BUILD}>:CDI_COSIM=1>)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../bios/generated/bios_identity.h")
    target_compile_definitions(CdiRuntime PRIVATE CDI_BIOS_IDENTITY_BUILD=1)
endif()
if(EXISTS "${CDI_GAME_GENERATED_DIR}/module_manifest.c")
    target_compile_definitions(CdiRuntime PRIVATE CDI_NATIVE_MODULES=1)
endif()

find_package(SDL2 CONFIG REQUIRED)
target_link_libraries(CdiRuntime PRIVATE SDL2::SDL2)

# Keep local acceptance evidence bound to the inputs actually linked. This
# sidecar contains hashes/paths only and is excluded from runtime archives.
get_target_property(PROVENANCE_SOURCES CdiRuntime SOURCES)
set(PROVENANCE_FILES "")
foreach(SOURCE IN LISTS PROVENANCE_SOURCES)
    get_filename_component(INPUT "${SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND PROVENANCE_FILES "${INPUT}")
endforeach()
file(GLOB_RECURSE PROVENANCE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/m68k-recomp-core/common/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../bios/generated/*.h"
    "${CDI_GAME_GENERATED_DIR}/*.h")
list(APPEND PROVENANCE_FILES ${PROVENANCE_HEADERS}
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_provenance.cmake"
    "${CMAKE_SOURCE_DIR}/CMakeLists.txt")
if(EXISTS "${CDI_GAME_GENERATED_DIR}/module_sources.cmake")
    list(APPEND PROVENANCE_FILES "${CDI_GAME_GENERATED_DIR}/module_sources.cmake")
endif()
set(PROVENANCE_INPUTS "${CMAKE_CURRENT_BINARY_DIR}/runtime-provenance-inputs.cmake")
file(GENERATE OUTPUT "${PROVENANCE_INPUTS}" CONTENT
    "set(PROVENANCE_FILES [==[${PROVENANCE_FILES}]==])\nset(PROVENANCE_COMPILER [==[${CMAKE_C_COMPILER};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION}]==])\nset(PROVENANCE_OPTIONS [==[${CMAKE_BUILD_TYPE};${CMAKE_C_FLAGS};${CMAKE_C_FLAGS_RELEASE};$<TARGET_PROPERTY:CdiRuntime,COMPILE_DEFINITIONS>;$<TARGET_PROPERTY:CdiRuntime,COMPILE_OPTIONS>]==])\n")
set_property(TARGET CdiRuntime APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_provenance.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
add_custom_command(TARGET CdiRuntime POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DPROVENANCE_INPUTS=${PROVENANCE_INPUTS}"
            "-DRUNTIME=$<TARGET_FILE:CdiRuntime>" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_provenance.cmake"
    COMMENT "Recording linked runtime input hashes")
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
    add_executable(SlaveTest tests/slave_test.c src/slave.c src/cdi_input.c)
    target_include_directories(SlaveTest PRIVATE include)
    add_test(NAME ikat_transport COMMAND SlaveTest)
    add_executable(CdiNativeTest tests/cdi_native_test.c src/cdi_native.c ../common/cdi_sha256.c)
    target_include_directories(CdiNativeTest PRIVATE include ../common)
    target_compile_definitions(CdiNativeTest PRIVATE CDI_NATIVE_MODULES=1)
    add_test(NAME native_module_binding COMMAND CdiNativeTest)
    add_executable(CdiSha256Test tests/cdi_sha256_test.c ../common/cdi_sha256.c)
    target_include_directories(CdiSha256Test PRIVATE ../common)
    add_test(NAME sha256 COMMAND CdiSha256Test)
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
