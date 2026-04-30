# 01_ut/CMakeLists.txt
# 各模块的单元测试

# ---- mem_pool UT ----
add_executable(test_mem_pool
    test_mem_pool.cpp
)
target_link_libraries(test_mem_pool PRIVATE mem_pool)
add_test(NAME test_mem_pool COMMAND test_mem_pool)

# ---- token_get_index UT ----
add_executable(token_get_index_ut
    token_get_index_ut.cpp
)
target_link_libraries(token_get_index_ut PRIVATE kvof_cpu)
add_test(NAME token_get_index_ut COMMAND token_get_index_ut)

# 一键运行全部 UT 的便捷目标:
#   cmake --build <build> --target run_all_ut
add_custom_target(run_all_ut
    COMMAND test_mem_pool
    COMMAND token_get_index_ut
    DEPENDS test_mem_pool token_get_index_ut
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)
