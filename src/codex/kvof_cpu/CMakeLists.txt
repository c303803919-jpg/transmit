if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(kvof_cpu_ut LANGUAGES CXX)
endif()

add_executable(token_get_index_ut
    token_get_index_ut.cpp
    token_get_index.cpp
    mock.cpp
)

target_compile_options(token_get_index_ut PRIVATE -O2 -Wall -Wextra)
target_include_directories(token_get_index_ut PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(run_token_get_index_ut
    COMMAND token_get_index_ut
    DEPENDS token_get_index_ut
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)