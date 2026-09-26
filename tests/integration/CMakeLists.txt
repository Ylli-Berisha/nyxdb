function(nyx_add_integration_test name path)
    add_executable(${name} ${path})
    target_link_libraries(${name} PRIVATE nyxdb_core GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

nyx_add_integration_test(scan_e2e_test executor/scan_e2e_test.cpp)
nyx_add_integration_test(pipeline_e2e_test executor/pipeline_e2e_test.cpp)

add_executable(server_test server/server_test.cpp)
target_link_libraries(server_test PRIVATE nyxdb_server_lib GTest::gtest_main)
add_test(NAME server_test COMMAND server_test)
set_tests_properties(server_test PROPERTIES TIMEOUT 200)

add_executable(replication_test server/replication_test.cpp)
target_link_libraries(replication_test PRIVATE nyxdb_server_lib GTest::gtest_main)
add_test(NAME replication_test COMMAND replication_test)
set_tests_properties(replication_test PROPERTIES TIMEOUT 200)

add_executable(replication_mp_test server/replication_mp_test.cpp)
target_link_libraries(replication_mp_test PRIVATE nyxdb_server_lib GTest::gtest_main)
target_compile_definitions(replication_mp_test PRIVATE
    NYXDB_SERVER_BIN="$<TARGET_FILE:nyxdb_server>")
add_test(NAME replication_mp_test COMMAND replication_mp_test)
set_tests_properties(replication_mp_test PROPERTIES TIMEOUT 300)

add_executable(sharding_mp_test server/sharding_mp_test.cpp)
target_link_libraries(sharding_mp_test PRIVATE nyxdb_server_lib GTest::gtest_main)
target_compile_definitions(sharding_mp_test PRIVATE
    NYXDB_SERVER_BIN="$<TARGET_FILE:nyxdb_server>")
add_test(NAME sharding_mp_test COMMAND sharding_mp_test)
set_tests_properties(sharding_mp_test PROPERTIES TIMEOUT 300)
