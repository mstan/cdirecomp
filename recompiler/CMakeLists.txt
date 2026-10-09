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
    src/module_emit.c
    src/module_entries.c
    src/module_seeds.c
    ../common/cdi_sha256.c
    ${CDI_FRONTEND_SOURCES}
)
target_include_directories(CdiRecomp PRIVATE src ../common ${M68K_RECOMP_CORE_INCLUDE_DIRS})

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
    ../common/cdi_sha256.c
    ${CDI_FRONTEND_SOURCES}
)
target_include_directories(CdiRecompBios PRIVATE src ../common ${M68K_RECOMP_CORE_INCLUDE_DIRS})
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

include(CTest)
if(BUILD_TESTING)
    add_executable(ModuleSeedsTest tests/module_seeds_test.c src/module_seeds.c)
    target_include_directories(ModuleSeedsTest PRIVATE src)
    add_test(NAME module_seeds COMMAND ModuleSeedsTest)
    add_executable(ModuleEntriesTest tests/module_entries_test.c src/module_entries.c)
    target_include_directories(ModuleEntriesTest PRIVATE src)
    add_test(NAME module_entries COMMAND ModuleEntriesTest)
    add_executable(ModuleCodegenFixture tests/module_codegen_fixture.c ${CDI_FRONTEND_SOURCES})
    target_include_directories(ModuleCodegenFixture PRIVATE src ${M68K_RECOMP_CORE_INCLUDE_DIRS})
    set(PIC_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/module-pic-fixture")
    add_custom_command(OUTPUT "${PIC_FIXTURE_DIR}/fixture_full.c" "${PIC_FIXTURE_DIR}/fixture_dispatch.c" "${PIC_FIXTURE_DIR}/fixture.bin"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PIC_FIXTURE_DIR}"
        COMMAND ModuleCodegenFixture "${PIC_FIXTURE_DIR}"
        DEPENDS ModuleCodegenFixture VERBATIM)
    add_executable(ModulePicTest tests/module_pic_test.c "${PIC_FIXTURE_DIR}/fixture_full.c" "${PIC_FIXTURE_DIR}/fixture_dispatch.c")
    target_include_directories(ModulePicTest PRIVATE ../runner/include)
    add_test(NAME native_pic_execution COMMAND ModulePicTest "${PIC_FIXTURE_DIR}/fixture.bin")
    add_test(NAME native_pic_rejects_falloff COMMAND ModuleCodegenFixture "${PIC_FIXTURE_DIR}" --test-falloff)
    add_executable(Os9ModuleTest tests/os9_module_test.c src/disc_parser.c)
    target_include_directories(Os9ModuleTest PRIVATE src)
    add_test(NAME os9_module_validation COMMAND Os9ModuleTest)
    add_executable(DiscFilesystemTest tests/disc_filesystem_test.c src/disc_parser.c)
    target_include_directories(DiscFilesystemTest PRIVATE src)
    add_test(NAME disc_filesystem COMMAND DiscFilesystemTest)
endif()
