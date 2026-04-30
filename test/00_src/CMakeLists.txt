# 00_src/CMakeLists.txt
# 把每个模块的源码组织成静态库，便于 01_ut 直接链接。

# ---- mem_pool 库 ----
add_library(mem_pool STATIC
    mem_pool.cpp
)
target_include_directories(mem_pool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(mem_pool PUBLIC Threads::Threads)

# ---- kvof_cpu (token_get_index + mock) 库 ----
# token_get_index.cpp：被测函数
# mock.cpp         ：元数据查询 + 数据搬移的 mock 实现
add_library(kvof_cpu STATIC
    token_get_index.cpp
    mock.cpp
)
target_include_directories(kvof_cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 如果环境里有 Ascend ACL，可在配置阶段打开：cmake -DKVOF_USE_ACL=ON ...
option(KVOF_USE_ACL "Link against real Ascend ACL runtime" OFF)
if(KVOF_USE_ACL)
    target_compile_definitions(kvof_cpu PRIVATE KVOF_USE_ACL=1)
    # 如有真实路径，可在此 target_link_libraries(kvof_cpu PRIVATE ascendcl)
endif()
