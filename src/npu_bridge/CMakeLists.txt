# NPU build for the CPU<->NPU KV-fetch bridge.
# Mirrors samples/0_helloworld so the kernel is compiled with the Ascend C
# tool-chain and main.cpp + the host service are linked against ACL.

cmake_minimum_required(VERSION 3.16.0)

project(npu_kv_bridge)

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
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_LIST_DIR}/out"
    CACHE STRING "path for install()" FORCE)

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

# --- NPU-side library: just the kernel ---
ascendc_library(npu_kv_kernels STATIC
    npu_kv_kernel.cpp
)

# --- Host service library (no NPU dependency) ---
add_library(npu_kv_host STATIC
    npu_kv_host.cpp
    ../query/kv_client.cpp
    ../query/kv_server.cpp
)
target_include_directories(npu_kv_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../query
)
target_compile_options(npu_kv_host PRIVATE -O2 -Wall -Wextra)

# --- Final binary: kernel + host service + ACL main ---
add_executable(npu_kv_main main.cpp)
target_link_libraries(npu_kv_main PRIVATE
    npu_kv_kernels
    npu_kv_host
)
