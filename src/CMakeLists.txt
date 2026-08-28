add_library(nyxdb_core STATIC
    common/logger.cpp
    storage/disk/disk_manager.cpp
    storage/disk/column_page.cpp
    storage/disk/column_file.cpp
    storage/memory/lru_k_replacer.cpp
    storage/memory/buffer_pool.cpp
)

target_include_directories(nyxdb_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nyxdb_core PUBLIC spdlog::spdlog)

add_executable(nyxdb main.cpp)
target_link_libraries(nyxdb PRIVATE nyxdb_core)
