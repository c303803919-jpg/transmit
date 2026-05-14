foreach(target kvof_server kvof_client)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} PRIVATE kvoF_transport)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
endforeach()
