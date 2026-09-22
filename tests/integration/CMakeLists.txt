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

add_executable(replication_test server/replication_test.cpp)
target_link_libraries(replication_test PRIVATE nyxdb_server_lib GTest::gtest_main)
add_test(NAME replication_test COMMAND replication_test)
set_tests_properties(replication_test PROPERTIES TIMEOUT 200)
