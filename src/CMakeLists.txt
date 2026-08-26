add_library(nyxdb_core STATIC
    common/logger.cpp
)

target_include_directories(nyxdb_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nyxdb_core PUBLIC spdlog::spdlog)
