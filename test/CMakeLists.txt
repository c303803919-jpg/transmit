cmake_minimum_required(VERSION 3.16)
project(KVoF_PR LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 通用编译选项
add_compile_options(-Wall -Wextra)

# CTest 支持 (ctest --test-dir build --output-on-failure)
enable_testing()

# 源码目录与单元测试目录
add_subdirectory(00_src)
add_subdirectory(01_ut)
