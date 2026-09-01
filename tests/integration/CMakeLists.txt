function(nyx_add_integration_test name path)
    add_executable(${name} ${path})
    target_link_libraries(${name} PRIVATE nyxdb_core GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

nyx_add_integration_test(scan_e2e_test executor/scan_e2e_test.cpp)
