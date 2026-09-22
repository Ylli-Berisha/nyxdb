add_library(nyxdb_core STATIC
    common/logger.cpp
    common/source_error.cpp
    storage/disk/disk_manager.cpp
    storage/disk/column_page.cpp
    storage/disk/column_file.cpp
    storage/disk/schema.cpp
    storage/disk/segment.cpp
    storage/disk/table.cpp
    storage/merge_worker.cpp
    storage/vacuum.cpp
    storage/disk/index_file.cpp
    storage/disk/btree_index.cpp
    storage/disk/constraint_file.cpp
    storage/memory/lru_k_replacer.cpp
    storage/memory/buffer_pool.cpp
    executor/table_scan.cpp
    executor/index_scan.cpp
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
    storage/wal/wal_writer.cpp
    storage/wal/wal_reader.cpp
    catalog/catalog.cpp
    binder/bound_ast.cpp
    binder/binder.cpp
    frontend/runner.cpp
    planner/planner.cpp
    database/database.cpp
)

target_include_directories(nyxdb_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nyxdb_core PUBLIC spdlog::spdlog nyxdb_warnings)

add_executable(nyxdb main.cpp)
target_link_libraries(nyxdb PRIVATE nyxdb_core)

add_library(nyxdb_server_lib STATIC
    server/wire.cpp
    server/session.cpp
    server/server.cpp
    server/replication_session.cpp
    replication/quic_client.cpp
    replication/follower_registry.cpp
    replication/wal_streamer.cpp
    replication/snapshot.cpp
    replication/wal_consumer.cpp
    replication/election.cpp
    replication/replication_manager.cpp
)
target_include_directories(nyxdb_server_lib SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/msquic/src/inc)
target_link_libraries(nyxdb_server_lib PUBLIC nyxdb_core msquic)

add_executable(nyxdb_server main_server.cpp)
target_link_libraries(nyxdb_server PRIVATE nyxdb_server_lib)
