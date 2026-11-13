cmake_minimum_required(VERSION 3.16)
project(CdiRecomp C)

set(CMAKE_C_STANDARD 11)

set(M68K_RECOMP_CORE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/m68k-recomp-core")
if(NOT EXISTS "${M68K_RECOMP_CORE_DIR}/cmake/M68kRecompCore.cmake")
    message(FATAL_ERROR "Initialize the shared frontend: git submodule update --init --recursive")
endif()
include("${M68K_RECOMP_CORE_DIR}/cmake/M68kRecompCore.cmake")
m68k_recomp_core_sources(M68K_RECOMP_CORE_SOURCES scc68070)
m68k_recomp_core_include_dirs(M68K_RECOMP_CORE_INCLUDE_DIRS scc68070)

set(CDI_FRONTEND_SOURCES
    ../common/m68k_image.c
    ${M68K_RECOMP_CORE_SOURCES}
    src/game_config.c
    src/toml.c
)

add_executable(CdiRecomp
    src/main_cdi.c
    ../common/disc_parser.c
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
    ../common/disc_parser.c
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
    tests/frontend_fixture.c
    tests/rom_fixture.c
    ${CDI_FRONTEND_SOURCES}
)
target_include_directories(CdiRecompFrontendFixture PRIVATE src ../common ${M68K_RECOMP_CORE_INCLUDE_DIRS})
set_target_properties(CdiRecompFrontendFixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(MSVC)
    target_compile_options(CdiRecompFrontendFixture PRIVATE /W3 /WX-)
endif()

include(CTest)
if(BUILD_TESTING)
    set_property(TARGET CdiRecompFrontendFixture PROPERTY EXCLUDE_FROM_ALL FALSE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME frontend_aliases COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_function_aliases.py"
        --recompiler "$<TARGET_FILE:CdiRecompFrontendFixture>")
    add_test(NAME bios_output_directory COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/bios_output_test.py"
        "$<TARGET_FILE:CdiRecompBios>")
    add_executable(ModuleSeedsTest tests/module_seeds_test.c src/module_seeds.c)
    target_include_directories(ModuleSeedsTest PRIVATE src)
    add_test(NAME module_seeds COMMAND ModuleSeedsTest)
    add_executable(ModuleEntriesTest tests/module_entries_test.c src/module_entries.c)
    target_include_directories(ModuleEntriesTest PRIVATE src)
    add_test(NAME module_entries COMMAND ModuleEntriesTest)
    add_executable(ModuleCodegenFixture tests/module_codegen_fixture.c ${CDI_FRONTEND_SOURCES})
    target_include_directories(ModuleCodegenFixture PRIVATE src ../common ${M68K_RECOMP_CORE_INCLUDE_DIRS})
    set(PIC_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/module-pic-fixture")
    add_custom_command(OUTPUT "${PIC_FIXTURE_DIR}/fixture_full.c" "${PIC_FIXTURE_DIR}/fixture_dispatch.c" "${PIC_FIXTURE_DIR}/fixture.bin"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PIC_FIXTURE_DIR}"
        COMMAND ModuleCodegenFixture "${PIC_FIXTURE_DIR}"
        DEPENDS ModuleCodegenFixture VERBATIM)
    add_executable(ModulePicTest tests/module_pic_test.c "${PIC_FIXTURE_DIR}/fixture_full.c" "${PIC_FIXTURE_DIR}/fixture_dispatch.c")
    target_include_directories(ModulePicTest PRIVATE ../runner/include)
    add_test(NAME native_pic_execution COMMAND ModulePicTest "${PIC_FIXTURE_DIR}/fixture.bin")
    add_test(NAME native_pic_rejects_falloff COMMAND ModuleCodegenFixture "${PIC_FIXTURE_DIR}" --test-falloff)
    set(PIC_DENSE_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/module-pic-dense-fixture")
    add_custom_command(OUTPUT "${PIC_DENSE_FIXTURE_DIR}/fixture_full.c" "${PIC_DENSE_FIXTURE_DIR}/fixture_dispatch.c" "${PIC_DENSE_FIXTURE_DIR}/fixture.bin"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PIC_DENSE_FIXTURE_DIR}"
        COMMAND ModuleCodegenFixture "${PIC_DENSE_FIXTURE_DIR}" --dense-entries
        DEPENDS ModuleCodegenFixture VERBATIM)
    add_executable(ModulePicDenseTest tests/module_pic_test.c
        "${PIC_DENSE_FIXTURE_DIR}/fixture_full.c" "${PIC_DENSE_FIXTURE_DIR}/fixture_dispatch.c")
    target_include_directories(ModulePicDenseTest PRIVATE ../runner/include)
    add_test(NAME native_pic_dense_execution COMMAND ModulePicDenseTest
        "${PIC_DENSE_FIXTURE_DIR}/fixture.bin" --expect-coarse)
    add_executable(Os9ModuleTest tests/os9_module_test.c ../common/disc_parser.c)
    target_include_directories(Os9ModuleTest PRIVATE ../common)
    add_test(NAME os9_module_validation COMMAND Os9ModuleTest)
    add_executable(DiscFilesystemTest tests/disc_filesystem_test.c ../common/disc_parser.c)
    target_include_directories(DiscFilesystemTest PRIVATE ../common)
    add_test(NAME disc_filesystem COMMAND DiscFilesystemTest)
endif()
