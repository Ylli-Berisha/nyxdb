add_library(nyxdb_core STATIC
    common/logger.cpp
    common/source_error.cpp
    storage/disk/disk_manager.cpp
    storage/disk/column_page.cpp
    storage/disk/column_file.cpp
    storage/disk/schema.cpp
    storage/disk/table.cpp
    storage/memory/lru_k_replacer.cpp
    storage/memory/buffer_pool.cpp
    executor/table_scan.cpp
    executor/expression.cpp
    executor/filter.cpp
    executor/project.cpp
    executor/limit.cpp
    executor/selection_vector.cpp
    executor/sort.cpp
    executor/hash_join.cpp
    executor/nested_loop_join.cpp
    executor/hash_aggregate.cpp
    parser/lexer.cpp
    parser/parser.cpp
    catalog/catalog.cpp
    binder/bound_ast.cpp
    binder/binder.cpp
    frontend/runner.cpp
)

target_include_directories(nyxdb_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nyxdb_core PUBLIC spdlog::spdlog)

add_executable(nyxdb main.cpp)
target_link_libraries(nyxdb PRIVATE nyxdb_core)
