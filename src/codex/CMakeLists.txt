cmake_minimum_required(VERSION 3.16.0)

project(kvof_npu_ut)

set(SOC_VERSION "Ascend910B2" CACHE STRING "system on chip type")
if(DEFINED ENV{USER} AND "$ENV{USER}" STREQUAL "root")
    set(DEFAULT_ASCEND_CANN_PACKAGE_PATH
        "/usr/local/Ascend/ascend-toolkit/latest"
        CACHE PATH "ASCEND CANN package default installation directory for root")
else()
    set(DEFAULT_ASCEND_CANN_PACKAGE_PATH
        "$ENV{HOME}/Ascend/ascend-toolkit/latest"
        CACHE PATH "ASCEND CANN package default installation directory")
endif()

if(DEFINED ASCEND_CANN_PACKAGE_PATH)
elseif(DEFINED ENV{ASCEND_HOME_PATH})
    set(ASCEND_CANN_PACKAGE_PATH "$ENV{ASCEND_HOME_PATH}"
        CACHE PATH "ASCEND CANN package installation directory" FORCE)
else()
    set(ASCEND_CANN_PACKAGE_PATH "${DEFAULT_ASCEND_CANN_PACKAGE_PATH}"
        CACHE PATH "ASCEND CANN package installation directory")
endif()

set(RUN_MODE "npu" CACHE STRING "run mode: npu")
set(CMAKE_BUILD_TYPE "Debug" CACHE STRING
    "Build type Release/Debug (default Debug)" FORCE)

if(EXISTS ${ASCEND_CANN_PACKAGE_PATH}/tools/tikcpp/ascendc_kernel_cmake)
    set(ASCENDC_CMAKE_DIR
        ${ASCEND_CANN_PACKAGE_PATH}/tools/tikcpp/ascendc_kernel_cmake)
elseif(EXISTS ${ASCEND_CANN_PACKAGE_PATH}/compiler/tikcpp/ascendc_kernel_cmake)
    set(ASCENDC_CMAKE_DIR
        ${ASCEND_CANN_PACKAGE_PATH}/compiler/tikcpp/ascendc_kernel_cmake)
elseif(EXISTS ${ASCEND_CANN_PACKAGE_PATH}/ascendc_devkit/tikcpp/samples/cmake)
    set(ASCENDC_CMAKE_DIR
        ${ASCEND_CANN_PACKAGE_PATH}/ascendc_devkit/tikcpp/samples/cmake)
else()
    message(FATAL_ERROR "ascendc_kernel_cmake does not exist; check CANN.")
endif()
include(${ASCENDC_CMAKE_DIR}/ascendc.cmake)

ascendc_library(kvof_npu_kernels STATIC
    npu_transfer_check_kernel.cpp
)

add_executable(npu_token_get_index_ut
    npu_token_get_index_ut.cpp
    kvof_cpu/mock.cpp
    kvof_cpu/token_get_index.cpp
)
target_compile_definitions(npu_token_get_index_ut PRIVATE KVOF_USE_ACL)
target_compile_options(npu_token_get_index_ut PRIVATE -O2 -Wall -Wextra)
target_include_directories(npu_token_get_index_ut PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/kvof_cpu
)
target_link_libraries(npu_token_get_index_ut PRIVATE
    kvof_npu_kernels
)

add_custom_target(run_npu_token_get_index_ut
    COMMAND npu_token_get_index_ut
    DEPENDS npu_token_get_index_ut
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)

add_subdirectory(kvof_cpu)
add_subdirectory(kvof_npu)
