cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED GLOBAL_CONFIG)
    project(KVoFTransport CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    enable_testing()
endif()

add_subdirectory(src)
add_subdirectory(ut)
