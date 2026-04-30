cmake_minimum_required(VERSION 3.16)
project(KVoF_PR LANGUAGES CXX)

# ----------------------------------------------------------------------
# 全局编译选项
# ----------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)
enable_testing()

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/00_src)
set(UT_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/01_ut)

# ----------------------------------------------------------------------
# 00_src: 各模块编成静态库（被 01_ut 链接）
# ----------------------------------------------------------------------

# mem_pool 模块
add_library(mem_pool STATIC
    ${SRC_DIR}/mem_pool.cpp
)
target_include_directories(mem_pool PUBLIC ${SRC_DIR})
find_package(Threads REQUIRED)
target_link_libraries(mem_pool PUBLIC Threads::Threads)

# kvof_cpu 模块（token_get_index + mock 元数据/搬移）
add_library(kvof_cpu STATIC
    ${SRC_DIR}/token_get_index.cpp
    ${SRC_DIR}/mock.cpp
)
target_include_directories(kvof_cpu PUBLIC ${SRC_DIR})

option(KVOF_USE_ACL "Link against real Ascend ACL runtime" OFF)
if(KVOF_USE_ACL)
    target_compile_definitions(kvof_cpu PRIVATE KVOF_USE_ACL=1)
    # target_link_libraries(kvof_cpu PRIVATE ascendcl)
endif()

# ----------------------------------------------------------------------
# 01_ut: 各模块单元测试可执行文件
# ----------------------------------------------------------------------

add_executable(test_mem_pool ${UT_DIR}/test_mem_pool.cpp)
target_link_libraries(test_mem_pool PRIVATE mem_pool)
add_test(NAME test_mem_pool COMMAND test_mem_pool)

add_executable(token_get_index_ut ${UT_DIR}/token_get_index_ut.cpp)
target_link_libraries(token_get_index_ut PRIVATE kvof_cpu)
add_test(NAME token_get_index_ut COMMAND token_get_index_ut)

# 一键运行全部 UT:
#   cmake --build <build> --target run_all_ut
add_custom_target(run_all_ut
    COMMAND $<TARGET_FILE:test_mem_pool>
    COMMAND $<TARGET_FILE:token_get_index_ut>
    DEPENDS test_mem_pool token_get_index_ut
    USES_TERMINAL
)
