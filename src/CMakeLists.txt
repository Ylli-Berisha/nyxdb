# nyxdb_core — grows as we add source files
add_library(nyxdb_core INTERFACE)

target_include_directories(nyxdb_core INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nyxdb_core INTERFACE spdlog::spdlog)
