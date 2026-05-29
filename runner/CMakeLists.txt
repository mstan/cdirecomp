cmake_minimum_required(VERSION 3.16)
project(CdiRuntime C)

set(CMAKE_C_STANDARD 11)

# Generated game code is dropped into hotelmario/generated/ by CdiRecomp and
# compiled in here. It #includes cdi_runtime.h + game_extras.h from runner/include.
# Empty until the loader milestone (MC-CDI-001), so the runtime builds standalone.
file(GLOB GEN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../hotelmario/generated/*.c")

add_executable(CdiRuntime
    src/main.c
    src/cdi_bus.c
    src/runtime.c
    src/mcd212.c
    src/cdic.c
    src/slave.c
    src/cdrtos.c
    src/debug_server.c
    ${GEN_SRCS}
)

target_include_directories(CdiRuntime PRIVATE
    include
    "${CMAKE_CURRENT_SOURCE_DIR}/../hotelmario/generated"
)

set_target_properties(CdiRuntime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
    target_compile_options(CdiRuntime PRIVATE /W3 /WX-)
endif()
