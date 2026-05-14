add_library(kvoF_transport STATIC
    kvoF_transport.cpp
    tcp_minimal_transport.cpp
    common/base/status.cpp)

target_include_directories(kvoF_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

if(TARGET asio_shared)
    target_link_libraries(kvoF_transport PUBLIC asio_shared glog::glog pthread)
else()
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
