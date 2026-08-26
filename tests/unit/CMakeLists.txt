add_executable(result_test common/result_test.cpp)
target_link_libraries(result_test PRIVATE nyxdb_core GTest::gtest_main)
add_test(NAME result_test COMMAND result_test)

add_executable(disk_manager_test storage/disk_manager_test.cpp)
target_link_libraries(disk_manager_test PRIVATE nyxdb_core GTest::gtest_main)
add_test(NAME disk_manager_test COMMAND disk_manager_test)
