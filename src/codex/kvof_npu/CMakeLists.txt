ascendc_library(kvof_get_npu_kernels STATIC
    kvof_get_notify_kernel.cpp
)

add_executable(kvof_get_npu_ut
    kvof_get_npu_ut.cpp
    kvof_get_cpu_handler.cpp
    ../kvof_cpu/mock.cpp
    ../kvof_cpu/token_get_index.cpp
)

target_compile_definitions(kvof_get_npu_ut PRIVATE KVOF_USE_ACL)
target_compile_options(kvof_get_npu_ut PRIVATE -O2 -Wall -Wextra)
target_include_directories(kvof_get_npu_ut PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../kvof_cpu
)

target_link_libraries(kvof_get_npu_ut PRIVATE
    kvof_get_npu_kernels
)

add_executable(kvof_get_cpu_handler_ut
    kvof_get_cpu_handler_ut.cpp
    kvof_get_cpu_handler.cpp
    ../kvof_cpu/mock.cpp
    ../kvof_cpu/token_get_index.cpp
)

target_compile_options(kvof_get_cpu_handler_ut PRIVATE -O2 -Wall -Wextra)
target_include_directories(kvof_get_cpu_handler_ut PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../kvof_cpu
)

add_custom_target(run_kvof_get_npu_ut
    COMMAND kvof_get_npu_ut
    DEPENDS kvof_get_npu_ut
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(run_kvof_get_cpu_handler_ut
    COMMAND kvof_get_cpu_handler_ut
    DEPENDS kvof_get_cpu_handler_ut
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)
