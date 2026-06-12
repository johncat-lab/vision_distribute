add_executable(comm_node
    main.cpp
    src/tcp_client.cpp
    src/vision_server.cpp
)

target_include_directories(comm_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(comm_node PRIVATE
    vision_logger
    vision_rpc
    ${OpenCV_LIBS}
)

if(rclcpp_FOUND)
    target_link_libraries(comm_node PRIVATE vision_interfaces__rosidl_typesupport_cpp)
endif()

install(TARGETS comm_node RUNTIME DESTINATION bin)
