cmake_minimum_required(VERSION 3.25)
project(CdiOracle CXX)

# The CeDImu CD-i emulator is the behavioral oracle (PRINCIPLES.md: ground
# truth). Its src/CDI tree builds a self-contained `CeDImu` core library with NO
# wxWidgets dependency (only the GUI needs wx) — we pull in just that target and
# link a headless driver that exposes the SAME TCP trace surface as CdiRuntime
# on port 4381, so native-vs-oracle first-divergence diffing is symmetric.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Bring in libCeDImu (external/CeDImu/src/CDI). LIBRARY_TYPE empty = static.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../external/CeDImu/src/CDI CDIcore)

add_executable(CdiOracle cdi_oracle.cpp)
target_link_libraries(CdiOracle PRIVATE CeDImu)
if(WIN32)
    target_link_libraries(CdiOracle PRIVATE ws2_32)
endif()

set_target_properties(CdiOracle PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
