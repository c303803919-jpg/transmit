find_package(GTest REQUIRED)

add_executable(tcp_kvoF_sw_test tcp_kvoF_sw_test.cpp)
target_link_libraries(tcp_kvoF_sw_test PRIVATE
    kvoF_transport
    GTest::gtest
    GTest::gtest_main)
target_include_directories(tcp_kvoF_sw_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_test(NAME tcp_kvoF_sw_test COMMAND tcp_kvoF_sw_test)
