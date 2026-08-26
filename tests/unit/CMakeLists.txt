add_executable(result_test common/result_test.cpp)
target_link_libraries(result_test PRIVATE nyxdb_core GTest::gtest_main)
add_test(NAME result_test COMMAND result_test)
