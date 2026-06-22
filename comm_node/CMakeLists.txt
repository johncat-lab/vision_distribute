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

install(TARGETS comm_node RUNTIME DESTINATION comm_node)

# ============================================================
# 使用 NodeContainer 框架的新通信节点版本
# ============================================================
add_executable(comm_node_v2
    main_new.cpp
    comm_node.cpp
    src/tcp_client.cpp
    src/vision_server.cpp
)

target_include_directories(comm_node_v2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(comm_node_v2 PRIVATE
    vision_logger
    vision_rpc
    ${OpenCV_LIBS}
)

if(rclcpp_FOUND)
    target_link_libraries(comm_node_v2 PRIVATE vision_interfaces__rosidl_typesupport_cpp)
endif()

install(TARGETS comm_node_v2 RUNTIME DESTINATION comm_node)

# 安装 comm_node 所需的配置文件
install(FILES
    ${CMAKE_SOURCE_DIR}/config/communication.xml
    ${CMAKE_SOURCE_DIR}/config/system_config.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zeromq.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_ros2.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zenoh.xml
    DESTINATION comm_node
)
