cmake_minimum_required(VERSION 3.20)
project(CdiRuntime C)

set(CMAKE_C_STANDARD 11)

# MC-CDI-016: differential co-simulation instrument (RAM/CPU state hashing,
# fault injection, cosim_full_ram_hash). See docs/COSIM-SPEC.md. Default ON —
# cosim_state.c is gated behind CDI_COSIM internally and compiles to an empty
# TU when this is OFF, so it is always safe to list in the source set below.
option(CDI_COSIM_BUILD "Build the MC-CDI-016 differential co-simulation instrument (RAM/CPU state hashing + fault injection)" ON)

# Same scanout contract, independent implementations. HLE consolidates source
# decode, matte/contribution/transparency and output into native color spans.
set(CDI_VIDEO_BACKEND "LLE" CACHE STRING "Scanout implementation: LLE or HLE")
set_property(CACHE CDI_VIDEO_BACKEND PROPERTY STRINGS LLE HLE)
if(CDI_VIDEO_BACKEND STREQUAL "HLE")
    set(CDI_VIDEO_HLE 1)
elseif(CDI_VIDEO_BACKEND STREQUAL "LLE")
    set(CDI_VIDEO_HLE 0)
else()
    message(FATAL_ERROR "CDI_VIDEO_BACKEND must be LLE or HLE")
endif()

# Complete native loader work has a separate build-time reference selection.
set(CDI_LOADER_BACKEND "LLE" CACHE STRING "Loader job implementation: LLE or HLE")
set_property(CACHE CDI_LOADER_BACKEND PROPERTY STRINGS LLE HLE)
if(CDI_LOADER_BACKEND STREQUAL "HLE")
    set(CDI_LOADER_HLE 1)
elseif(CDI_LOADER_BACKEND STREQUAL "LLE")
    set(CDI_LOADER_HLE 0)
else()
    message(FATAL_ERROR "CDI_LOADER_BACKEND must be LLE or HLE")
endif()

# Guest drawing preparation has an independent build-time faithful choice.
set(CDI_DRAWING_BACKEND "LLE" CACHE STRING "Deferred drawing preparation: LLE or HLE")
set_property(CACHE CDI_DRAWING_BACKEND PROPERTY STRINGS LLE HLE)
if(CDI_DRAWING_BACKEND STREQUAL "HLE")
    set(CDI_DRAWING_HLE 1)
elseif(CDI_DRAWING_BACKEND STREQUAL "LLE")
    set(CDI_DRAWING_HLE 0)
else()
    message(FATAL_ERROR "CDI_DRAWING_BACKEND must be LLE or HLE")
endif()

# Complete native memory operations retain an independent faithful selection.
set(CDI_MEMORY_BACKEND "LLE" CACHE STRING "Memory operation jobs: LLE or HLE")
set_property(CACHE CDI_MEMORY_BACKEND PROPERTY STRINGS LLE HLE)
if(CDI_MEMORY_BACKEND STREQUAL "HLE")
    set(CDI_MEMORY_HLE 1)
elseif(CDI_MEMORY_BACKEND STREQUAL "LLE")
    set(CDI_MEMORY_HLE 0)
else()
    message(FATAL_ERROR "CDI_MEMORY_BACKEND must be LLE or HLE")
endif()

set(CDI_SERVICE_BACKEND "LLE" CACHE STRING "Process lookup/publication jobs: LLE or HLE")
set_property(CACHE CDI_SERVICE_BACKEND PROPERTY STRINGS LLE HLE)
if(CDI_SERVICE_BACKEND STREQUAL "HLE")
    set(CDI_SERVICE_HLE 1)
elseif(CDI_SERVICE_BACKEND STREQUAL "LLE")
    set(CDI_SERVICE_HLE 0)
else()
    message(FATAL_ERROR "CDI_SERVICE_BACKEND must be LLE or HLE")
endif()

set(CDI_STREAMING_BACKEND "LLE" CACHE STRING "CIAP streaming progression: LLE or HLE")
set_property(CACHE CDI_STREAMING_BACKEND PROPERTY STRINGS LLE HLE)
if(CDI_STREAMING_BACKEND STREQUAL "HLE")
    set(CDI_STREAMING_HLE 1)
elseif(CDI_STREAMING_BACKEND STREQUAL "LLE")
    set(CDI_STREAMING_HLE 0)
else()
    message(FATAL_ERROR "CDI_STREAMING_BACKEND must be LLE or HLE")
endif()
set_source_files_properties(src/cdic.c PROPERTIES
    COMPILE_DEFINITIONS "CDI_STREAMING_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_STREAMING>>")
set(CDI_BOARD_BACKEND "LLE" CACHE STRING "Stopped-CPU whole-board jobs: LLE or HLE")
set_property(CACHE CDI_BOARD_BACKEND PROPERTY STRINGS LLE HLE)
if(CDI_BOARD_BACKEND STREQUAL "HLE")
    set(CDI_BOARD_HLE 1)
elseif(CDI_BOARD_BACKEND STREQUAL "LLE")
    set(CDI_BOARD_HLE 0)
else()
    message(FATAL_ERROR "CDI_BOARD_BACKEND must be LLE or HLE")
endif()
set_source_files_properties(src/mcd212.c PROPERTIES
    COMPILE_DEFINITIONS "CDI_BOARD_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_BOARD>>")

set(CDI_GPU_BACKEND "LLE" CACHE STRING "Owned raster presentation: LLE or HLE")
set_property(CACHE CDI_GPU_BACKEND PROPERTY STRINGS LLE HLE)
if(CDI_GPU_BACKEND STREQUAL "HLE")
    set(CDI_GPU_HLE 1)
elseif(CDI_GPU_BACKEND STREQUAL "LLE")
    set(CDI_GPU_HLE 0)
else()
    message(FATAL_ERROR "CDI_GPU_BACKEND must be LLE or HLE")
endif()
set_source_files_properties(src/cdi_frontend.c src/cdi_gpu.c PROPERTIES
    COMPILE_DEFINITIONS "CDI_GPU_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_GPU>>")

find_package(SDL2 CONFIG REQUIRED)
# Component parity tests select a backend even when no player is built.
set_source_files_properties(src/mcd212_video.c PROPERTIES
    COMPILE_DEFINITIONS "CDI_VIDEO_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_SCANOUT>>")
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/CdiProvenance.cmake")
option(CDI_BUILD_RUNTIME "Build a native player with generated BIOS sources" ON)
if(NOT DEFINED CDI_RUNTIME_TARGET)
    set(CDI_RUNTIME_TARGET CdiRuntime)
endif()
if(NOT DEFINED CDI_BIOS_GENERATED_DIR)
    set(CDI_BIOS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/bios" CACHE PATH
        "Directory containing generated CD-RTOS sources and bios_identity.h")
endif()
if(NOT DEFINED CDI_GAME_GENERATED_DIR)
    set(CDI_GAME_GENERATED_DIR "" CACHE PATH "Optional generated native OS-9 modules")
endif()

if(CDI_BUILD_RUNTIME)
if(NOT EXISTS "${CDI_BIOS_GENERATED_DIR}/cdrtos_full.c" OR
   NOT EXISTS "${CDI_BIOS_GENERATED_DIR}/cdrtos_dispatch.c" OR
   NOT EXISTS "${CDI_BIOS_GENERATED_DIR}/bios_identity.h")
    message(FATAL_ERROR "Generate the BIOS first with CdiRecompBios <rom> --emit --out <directory>, then set CDI_BIOS_GENERATED_DIR")
endif()
set(BIOS_GEN_SRCS "${CDI_BIOS_GENERATED_DIR}/cdrtos_full.c"
                  "${CDI_BIOS_GENERATED_DIR}/cdrtos_dispatch.c")
set(GAME_GEN_SRCS "")
if(CDI_GAME_GENERATED_DIR)
    if(NOT EXISTS "${CDI_GAME_GENERATED_DIR}/module_manifest.c" OR
       NOT EXISTS "${CDI_GAME_GENERATED_DIR}/module_sources.cmake")
        message(FATAL_ERROR "Regenerate native modules: module manifest/source list is missing")
    endif()
    include("${CDI_GAME_GENERATED_DIR}/module_sources.cmake")
    foreach(MODULE_SOURCE IN LISTS CDI_NATIVE_MODULE_FILES)
        list(APPEND GAME_GEN_SRCS "${CDI_GAME_GENERATED_DIR}/${MODULE_SOURCE}")
    endforeach()
endif()

if(NOT TARGET ${CDI_RUNTIME_TARGET})
    add_executable(${CDI_RUNTIME_TARGET})
endif()
target_sources(${CDI_RUNTIME_TARGET} PRIVATE
    src/main.c
    src/cdi_bus.c
    src/cdi_native.c
    src/cdi_loader_jobs.c
    src/cdi_drawing_jobs.c
    src/cdi_memory_jobs.c
    src/cdi_service_jobs.c
    ../common/cdi_sha256.c
    src/runtime.c
    src/mcd212.c
    src/mcd212_video.c
    src/cdi_frontend.c
    src/cdi_render_profile.c
    src/cdi_raster_jobs.c
    src/cdi_gpu.c
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
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/m68k_image.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/disc_parser.c"
    ${BIOS_GEN_SRCS}
    ${GAME_GEN_SRCS}
)

# GCC's -O3 expansion of the ~450k-line generated ROM can exhaust the MinGW
# assembler even on otherwise roomy developer machines. -O2 keeps the runtime
# optimized while making clean Release builds deterministic and materially
# smaller at the compiler/assembler boundary.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        "${CDI_BIOS_GENERATED_DIR}/cdrtos_full.c"
        TARGET_DIRECTORY ${CDI_RUNTIME_TARGET} PROPERTIES COMPILE_OPTIONS "-O2"
    )
endif()

target_include_directories(${CDI_RUNTIME_TARGET} PRIVATE
    include
    ../common
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/m68k-recomp-core/common"
    "${CDI_BIOS_GENERATED_DIR}"
    "${CDI_GAME_GENERATED_DIR}"
)

set_target_properties(${CDI_RUNTIME_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
    target_compile_options(${CDI_RUNTIME_TARGET} PRIVATE /W3 /WX-)
endif()

target_compile_definitions(${CDI_RUNTIME_TARGET} PRIVATE $<$<BOOL:${CDI_COSIM_BUILD}>:CDI_COSIM=1>)
# Restrict the selection macro to the implementation and startup identity.
# Switching backends must not rebuild the generated CPU/game translation units.
get_target_property(CDI_RUNTIME_SOURCE_DIR ${CDI_RUNTIME_TARGET} SOURCE_DIR)
set_source_files_properties(src/mcd212_video.c src/main.c
    TARGET_DIRECTORY ${CDI_RUNTIME_TARGET} PROPERTIES
    COMPILE_DEFINITIONS "CDI_VIDEO_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_SCANOUT>>")
set_property(TARGET ${CDI_RUNTIME_TARGET} PROPERTY CDI_NATIVE_SCANOUT ${CDI_VIDEO_HLE})
set_property(SOURCE src/main.c DIRECTORY "${CDI_RUNTIME_SOURCE_DIR}" APPEND PROPERTY COMPILE_DEFINITIONS
    "CDI_BOARD_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_BOARD>>")
set_source_files_properties(src/cdi_loader_jobs.c
    "${CDI_BIOS_GENERATED_DIR}/cdrtos_dispatch.c"
    TARGET_DIRECTORY ${CDI_RUNTIME_TARGET}
    PROPERTIES COMPILE_DEFINITIONS "CDI_LOADER_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_LOADER>>")
set_property(TARGET ${CDI_RUNTIME_TARGET} PROPERTY CDI_NATIVE_LOADER ${CDI_LOADER_HLE})
set_source_files_properties(src/cdi_drawing_jobs.c
    TARGET_DIRECTORY ${CDI_RUNTIME_TARGET} PROPERTIES
    COMPILE_DEFINITIONS "CDI_DRAWING_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_DRAWING>>")
set_property(SOURCE "${CDI_BIOS_GENERATED_DIR}/cdrtos_dispatch.c"
    DIRECTORY "${CDI_RUNTIME_SOURCE_DIR}" APPEND PROPERTY COMPILE_DEFINITIONS "CDI_DRAWING_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_DRAWING>>")
set_property(TARGET ${CDI_RUNTIME_TARGET} PROPERTY CDI_NATIVE_DRAWING ${CDI_DRAWING_HLE})
set_source_files_properties(src/cdi_memory_jobs.c
    TARGET_DIRECTORY ${CDI_RUNTIME_TARGET} PROPERTIES
    COMPILE_DEFINITIONS "CDI_MEMORY_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_MEMORY>>")
set_property(SOURCE "${CDI_BIOS_GENERATED_DIR}/cdrtos_dispatch.c"
    DIRECTORY "${CDI_RUNTIME_SOURCE_DIR}" APPEND PROPERTY COMPILE_DEFINITIONS "CDI_MEMORY_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_MEMORY>>")
set_property(TARGET ${CDI_RUNTIME_TARGET} PROPERTY CDI_NATIVE_MEMORY ${CDI_MEMORY_HLE})
set_source_files_properties(src/cdi_service_jobs.c src/cdi_native.c
    TARGET_DIRECTORY ${CDI_RUNTIME_TARGET} PROPERTIES
    COMPILE_DEFINITIONS "CDI_SERVICE_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_SERVICE>>")
set_property(SOURCE "${CDI_BIOS_GENERATED_DIR}/cdrtos_dispatch.c"
    DIRECTORY "${CDI_RUNTIME_SOURCE_DIR}" APPEND PROPERTY COMPILE_DEFINITIONS "CDI_SERVICE_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_SERVICE>>")
set_property(TARGET ${CDI_RUNTIME_TARGET} PROPERTY CDI_NATIVE_SERVICE ${CDI_SERVICE_HLE})
set_property(TARGET ${CDI_RUNTIME_TARGET} PROPERTY CDI_NATIVE_STREAMING ${CDI_STREAMING_HLE})
set_property(TARGET ${CDI_RUNTIME_TARGET} PROPERTY CDI_NATIVE_BOARD ${CDI_BOARD_HLE})
set_property(TARGET ${CDI_RUNTIME_TARGET} PROPERTY CDI_NATIVE_GPU ${CDI_GPU_HLE})
if(EXISTS "${CDI_BIOS_GENERATED_DIR}/bios_identity.h")
    target_compile_definitions(${CDI_RUNTIME_TARGET} PRIVATE CDI_BIOS_IDENTITY_BUILD=1)
endif()
if(EXISTS "${CDI_GAME_GENERATED_DIR}/module_manifest.c")
    target_compile_definitions(${CDI_RUNTIME_TARGET} PRIVATE CDI_NATIVE_MODULES=1)
endif()

target_link_libraries(${CDI_RUNTIME_TARGET} PRIVATE SDL2::SDL2)

set_target_properties(${CDI_RUNTIME_TARGET} PROPERTIES
    CDI_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    CDI_BIOS_DIR "${CDI_BIOS_GENERATED_DIR}"
    CDI_MODULE_DIR "${CDI_GAME_GENERATED_DIR}"
    CDI_BACKEND_IDENTITY "scanout=${CDI_VIDEO_BACKEND};loader=${CDI_LOADER_BACKEND};drawing=${CDI_DRAWING_BACKEND};memory=${CDI_MEMORY_BACKEND};service=${CDI_SERVICE_BACKEND};streaming=${CDI_STREAMING_BACKEND};board=${CDI_BOARD_BACKEND};gpu=${CDI_GPU_BACKEND}")
set_source_files_properties(src/cdic.c TARGET_DIRECTORY ${CDI_RUNTIME_TARGET} PROPERTIES
    COMPILE_DEFINITIONS "CDI_STREAMING_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_STREAMING>>")
set_source_files_properties(src/mcd212.c TARGET_DIRECTORY ${CDI_RUNTIME_TARGET} PROPERTIES
    COMPILE_DEFINITIONS "CDI_BOARD_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_BOARD>>")
set_source_files_properties(src/cdi_frontend.c src/cdi_gpu.c TARGET_DIRECTORY ${CDI_RUNTIME_TARGET} PROPERTIES
    COMPILE_DEFINITIONS "CDI_GPU_HLE=$<BOOL:$<TARGET_PROPERTY:CDI_NATIVE_GPU>>")

# Debug server (debug_server.c): winsock on Windows, pthreads elsewhere.
if(WIN32)
    target_link_libraries(${CDI_RUNTIME_TARGET} PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(${CDI_RUNTIME_TARGET} PRIVATE Threads::Threads)
endif()

if(NOT CDI_DEFER_PROVENANCE)
    cdirecomp_finalize_player(${CDI_RUNTIME_TARGET})
endif()
endif() # CDI_BUILD_RUNTIME

include(CTest)
if(BUILD_TESTING)
    add_executable(CdiLoaderMathTest tests/cdi_loader_math_test.c)
    target_include_directories(CdiLoaderMathTest PRIVATE include)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../bios/cdi490a.rom")
        add_test(NAME loader_crc_polynomial COMMAND CdiLoaderMathTest
            "${CMAKE_CURRENT_SOURCE_DIR}/../bios/cdi490a.rom")
    endif()
    add_executable(SlaveTest tests/slave_test.c src/slave.c src/cdi_input.c)
    target_include_directories(SlaveTest PRIVATE include)
    add_test(NAME ikat_transport COMMAND SlaveTest)
    add_executable(CdiNativeTest tests/cdi_native_test.c src/cdi_native.c ../common/cdi_sha256.c)
    target_include_directories(CdiNativeTest PRIVATE include ../common)
    target_compile_definitions(CdiNativeTest PRIVATE CDI_NATIVE_MODULES=1)
    add_test(NAME native_module_binding COMMAND CdiNativeTest)
    add_executable(CdiNativeIndexedTest tests/cdi_native_test.c src/cdi_native.c ../common/cdi_sha256.c)
    target_include_directories(CdiNativeIndexedTest PRIVATE include ../common)
    target_compile_definitions(CdiNativeIndexedTest PRIVATE CDI_NATIVE_MODULES=1)
    set_property(TARGET CdiNativeIndexedTest PROPERTY CDI_NATIVE_SERVICE 1)
    add_test(NAME native_indexed_publication COMMAND CdiNativeIndexedTest)
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
        src/cdi_raster_jobs.c
    )
    target_include_directories(Mcd212VideoTest PRIVATE include)
    set_property(TARGET Mcd212VideoTest PROPERTY CDI_NATIVE_SCANOUT ${CDI_VIDEO_HLE})
    add_test(NAME mcd212_video COMMAND Mcd212VideoTest)

    # Link independent reference and native machines into one differential
    # test. Production never executes the reference alongside an HLE job.
    add_library(Mcd212VideoReference OBJECT src/mcd212_video.c)
    target_compile_definitions(Mcd212VideoReference PRIVATE CDI_RASTER_REFERENCE=1)
    target_include_directories(Mcd212VideoReference PRIVATE include)
    foreach(API reset capture_raster control begin_frame draw_line end_frame copy_frame copy_changed_frame
                frame_hash debug_cursor debug_state debug_clut debug_plane_row
                debug_verdict_row)
        target_compile_definitions(Mcd212VideoReference PRIVATE
            mcd212_video_${API}=lle_mcd212_video_${API})
    endforeach()
    add_executable(Mcd212VideoParityTest tests/mcd212_video_parity_test.c
        src/mcd212_video.c src/cdi_raster_jobs.c $<TARGET_OBJECTS:Mcd212VideoReference>)
    target_include_directories(Mcd212VideoParityTest PRIVATE include)
    set_property(TARGET Mcd212VideoParityTest PROPERTY CDI_NATIVE_SCANOUT 1)
    add_test(NAME mcd212_scanout_contract COMMAND Mcd212VideoParityTest)

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
    add_executable(CdicStreamingTest tests/cdic_test.c src/cdic.c)
    target_include_directories(CdicStreamingTest PRIVATE include)
    set_property(TARGET CdicStreamingTest PROPERTY CDI_NATIVE_STREAMING 1)
    add_test(NAME cdic_streaming COMMAND CdicStreamingTest)

    add_executable(PlayerConfigTest
        tests/player_config_test.c
        src/player_config.c
    )
    target_include_directories(PlayerConfigTest PRIVATE include)
    target_link_libraries(PlayerConfigTest PRIVATE SDL2::SDL2)
    add_test(NAME player_config COMMAND PlayerConfigTest)
endif()
