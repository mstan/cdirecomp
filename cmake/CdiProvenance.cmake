# Call after attaching consumer/launcher sources. The post-link hook is owned
# by the runtime target's creation directory, while its final input set is here.
function(cdirecomp_finalize_player target)
    get_target_property(TARGET_SOURCE_DIR ${target} SOURCE_DIR)
    get_target_property(SOURCE_DIR ${target} CDI_ENGINE_SOURCE_DIR)
    get_target_property(BINARY_DIR ${target} BINARY_DIR)
    get_target_property(SOURCES ${target} SOURCES)
    get_target_property(BIOS_DIR ${target} CDI_BIOS_DIR)
    get_target_property(MODULE_DIR ${target} CDI_MODULE_DIR)
    get_target_property(BACKENDS ${target} CDI_BACKEND_IDENTITY)
    set(PROVENANCE_FILES "")
    foreach(SOURCE IN LISTS SOURCES)
        get_filename_component(INPUT "${SOURCE}" ABSOLUTE BASE_DIR "${TARGET_SOURCE_DIR}")
        list(APPEND PROVENANCE_FILES "${INPUT}")
    endforeach()
    file(GLOB HEADERS CONFIGURE_DEPENDS
        "${SOURCE_DIR}/include/*.h" "${SOURCE_DIR}/src/*.h"
        "${SOURCE_DIR}/../common/*.h"
        "${SOURCE_DIR}/../external/m68k-recomp-core/common/*.h" "${BIOS_DIR}/*.h")
    if(MODULE_DIR)
        file(GLOB MODULE_HEADERS CONFIGURE_DEPENDS "${MODULE_DIR}/*.h")
        list(APPEND HEADERS ${MODULE_HEADERS} "${MODULE_DIR}/module_sources.cmake")
    endif()
    if(RECOMP_UI_ROOT)
        file(GLOB_RECURSE UI_HEADERS CONFIGURE_DEPENDS "${RECOMP_UI_ROOT}/src/*.h")
        list(APPEND HEADERS ${UI_HEADERS} "${RECOMP_UI_ROOT}/recomp_ui.cmake")
    endif()
    list(APPEND PROVENANCE_FILES ${HEADERS}
        "${SOURCE_DIR}/CMakeLists.txt" "${SOURCE_DIR}/../cmake/CdiProvenance.cmake"
        "${SOURCE_DIR}/../cmake/CdiGame.cmake" "${SOURCE_DIR}/../tools/build_provenance.cmake"
        "${CMAKE_SOURCE_DIR}/CMakeLists.txt")
    file(GENERATE OUTPUT "${BINARY_DIR}/runtime-provenance-inputs.cmake" CONTENT
        "set(PROVENANCE_FILES [==[${PROVENANCE_FILES}]==])\nset(PROVENANCE_COMPILER [==[${CMAKE_C_COMPILER};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION}]==])\nset(PROVENANCE_OPTIONS [==[${CMAKE_BUILD_TYPE};${CMAKE_C_FLAGS};${CMAKE_C_FLAGS_RELEASE};${BACKENDS};$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>;$<TARGET_PROPERTY:${target},COMPILE_OPTIONS>]==])\n")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${SOURCE_DIR}/../tools/build_provenance.cmake" "${SOURCE_DIR}/CMakeLists.txt"
        "${SOURCE_DIR}/../cmake/CdiProvenance.cmake")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DPROVENANCE_INPUTS=${BINARY_DIR}/runtime-provenance-inputs.cmake"
            "-DRUNTIME=$<TARGET_FILE:${target}>" -P "${SOURCE_DIR}/../tools/build_provenance.cmake"
        COMMENT "Recording final player and launcher input hashes" VERBATIM)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:${target}> VERBATIM)
    endif()
endfunction()
