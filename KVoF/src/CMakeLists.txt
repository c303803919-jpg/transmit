if(NOT TARGET asio_shared)
    # Standalone: include Status implementation from parent project
    set(KVOR_STATUS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/base/status.cpp)
else()
    set(KVOR_STATUS_SRC "")
endif()

add_library(kvoF_transport STATIC
    kvoF_transport.cpp
    tcp_minimal_transport.cpp
    ${KVOR_STATUS_SRC})

target_include_directories(kvoF_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${KVoF_PARENT_INCLUDE})

if(TARGET asio_shared)
    # Building inside mooncake project: use pre-built asio shared lib
    target_link_libraries(kvoF_transport PUBLIC asio_shared glog::glog pthread)
else()
    # Standalone build: header-only asio from system
    find_package(asio QUIET)
    if(asio_FOUND)
        target_include_directories(kvoF_transport PUBLIC ${asio_INCLUDE_DIRS})
    else()
        find_path(ASIO_INCLUDE_DIR asio.hpp REQUIRED)
        target_include_directories(kvoF_transport PUBLIC ${ASIO_INCLUDE_DIR})
    endif()
    find_package(glog REQUIRED)
    target_link_libraries(kvoF_transport PUBLIC glog::glog pthread)
endif()
