cmake_minimum_required(VERSION 3.20)
project(cdirecomp LANGUAGES C)

include(CTest)
option(CDI_BUILD_RUNTIME "Build the player (requires generated BIOS sources)" OFF)
add_subdirectory(recompiler)
add_subdirectory(runner)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME release_asset_manifest COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_assets.py")
endif()
