cmake_minimum_required(VERSION 3.20)
project(nyxdb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)
add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)

set(QUIC_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_TEST  OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_PERF  OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/msquic EXCLUDE_FROM_ALL)

add_library(nyxdb_warnings INTERFACE)
target_compile_options(nyxdb_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-DNDEBUG>
    $<$<CONFIG:Release>:-march=native>
)
add_link_options(
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
