add_executable(camera_node
    main.cpp
    src/hik_camera.cpp
)

target_include_directories(camera_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HIK_SDK_INCLUDE_DIR}
)

target_link_libraries(camera_node PRIVATE
    vision_logger
    vision_rpc
    ${HIK_MVCAMERACONTROL_LIB}
    ${OpenCV_LIBS}
)

if(rclcpp_FOUND)
    target_link_libraries(camera_node PRIVATE vision_interfaces__rosidl_typesupport_cpp)
endif()

install(TARGETS camera_node RUNTIME DESTINATION camera_node)

# ============================================================
# 使用 NodeContainer 框架的新相机节点版本
# ============================================================
add_executable(camera_node_v2
    main_new.cpp
    camera_node.cpp
    src/hik_camera.cpp
)

target_include_directories(camera_node_v2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HIK_SDK_INCLUDE_DIR}
)

target_link_libraries(camera_node_v2 PRIVATE
    vision_logger
    vision_rpc
    vision_dag
    ${HIK_MVCAMERACONTROL_LIB}
    ${OpenCV_LIBS}
)

if(rclcpp_FOUND)
    target_link_libraries(camera_node_v2 PRIVATE vision_interfaces__rosidl_typesupport_cpp)
endif()

install(TARGETS camera_node_v2 RUNTIME DESTINATION camera_node)

# 安装 camera_node 所需的配置文件
install(FILES
    ${CMAKE_SOURCE_DIR}/config/camera_config.xml
    ${CMAKE_SOURCE_DIR}/config/system_config.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zeromq.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_ros2.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zenoh.xml
    DESTINATION camera_node
)
