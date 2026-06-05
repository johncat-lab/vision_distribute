add_executable(comm_node
    src/main.cpp
    src/tcp_client.cpp
    src/vision_server.cpp
)

target_include_directories(comm_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(comm_node PRIVATE
    vision_rpc
    vision_core
    ${OpenCV_LIBS}
)

install(TARGETS comm_node RUNTIME DESTINATION bin)
