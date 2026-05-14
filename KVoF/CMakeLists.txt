cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED GLOBAL_CONFIG)
    # Standalone build (outside mooncake project)
    project(KVoFTransport CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    enable_testing()
    # Point to the parent project's include/ for common/base/status.h
    set(KVoF_PARENT_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
else()
    # Part of mooncake build
    set(KVoF_PARENT_INCLUDE ${CMAKE_SOURCE_DIR}/include)
endif()

add_subdirectory(src)
add_subdirectory(ut)
