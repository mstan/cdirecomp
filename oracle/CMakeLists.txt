cmake_minimum_required(VERSION 3.25)
project(CdiOracle CXX)

# The CeDImu CD-i emulator is the behavioral oracle (PRINCIPLES.md: ground
# truth). Its src/CDI tree builds a self-contained `CeDImu` core library with NO
# wxWidgets dependency (only the GUI needs wx) — we pull in just that target and
# link a headless driver that exposes the SAME TCP trace surface as CdiRuntime
# on port 4381, so native-vs-oracle first-divergence diffing is symmetric.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MC-CDI-016 differential co-sim instrument (docs/COSIM-SPEC.md). ON (default)
# compiles in the oracle's per-instruction RAM/register hashing + fault
# injection (oracle/cosim_state.cpp) AND defines CDI_COSIM for the vendored
# CeDImu library too, so its ONE RAM-write choke point (MCD212::SetByte/
# SetWord, cores/MCD212/DRAMInterface.cpp) compiles in the dirty-page mark
# hook. OFF builds a stock CeDImu with zero cosim footprint.
option(CDI_COSIM_BUILD "Build the MC-CDI-016 differential co-sim instrumentation" ON)

# Bring in libCeDImu (external/CeDImu/src/CDI). LIBRARY_TYPE empty = static.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../external/CeDImu/src/CDI CDIcore)

add_executable(CdiOracle cdi_oracle.cpp cosim_state.cpp)
target_link_libraries(CdiOracle PRIVATE CeDImu)
# The literal cdi490 ROM opens pt1driv on IKAT channel A. Keep this oracle-
# harness correction independent of the expensive RAM-hash instrument so the
# behavioral UI observer can also be built with CDI_COSIM_BUILD=OFF.
target_compile_definitions(CeDImu PRIVATE CDI_ORACLE_LITERAL_IKAT=1)
if(WIN32)
    target_link_libraries(CdiOracle PRIVATE ws2_32)
endif()

if(CDI_COSIM_BUILD)
    target_compile_definitions(CdiOracle PRIVATE CDI_COSIM=1)
    target_compile_definitions(CeDImu PRIVATE CDI_COSIM=1)
endif()

set_target_properties(CdiOracle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
