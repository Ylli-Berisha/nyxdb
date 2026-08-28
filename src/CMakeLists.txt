add_library(nyxdb_core STATIC
    common/logger.cpp
    storage/disk_manager.cpp
    storage/lru_k_replacer.cpp
    storage/buffer_pool.cpp
    storage/column_page.cpp
    storage/column_file.cpp
)

target_include_directories(nyxdb_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nyxdb_core PUBLIC spdlog::spdlog)

add_executable(nyxdb main.cpp)
target_link_libraries(nyxdb PRIVATE nyxdb_core)
