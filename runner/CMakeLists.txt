cmake_minimum_required(VERSION 3.16)
project(CdiRuntime C)

set(CMAKE_C_STANDARD 11)

# Generated C is dropped in by the recompiler and compiled in here. It
# #includes cdi_runtime.h + game_extras.h from runner/include.
#   - bios/generated/*.c     : the recompiled CD-RTOS system ROM (Phase 1, boots).
#   - hotelmario/generated/*.c : the recompiled game (Phase 3, empty until then).
# OS-first: the BIOS is what we boot to the player shell before any game work.
file(GLOB BIOS_GEN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../bios/generated/*.c")
file(GLOB GAME_GEN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../hotelmario/generated/*.c")

add_executable(CdiRuntime
    src/main.c
    src/cdi_bus.c
    src/runtime.c
    src/mcd212.c
    src/cdic.c
    src/slave.c
    src/periph.c
    src/cdi_nvram.c
    src/cdrtos.c
    src/debug_server.c
    src/m68k_interp.c
    # Clean-room hybrid interpreter (MC-CDI-011) reuses the recompiler's own
    # decoder for parity-by-construction with the static C emitter.
    "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src/m68k_decoder.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../recompiler/src/rom_parser.c"
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

# Debug server (debug_server.c): winsock on Windows, pthreads elsewhere.
if(WIN32)
    target_link_libraries(CdiRuntime PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(CdiRuntime PRIVATE Threads::Threads)
endif()
