add_executable(image_publisher_node
    main.cpp
)

target_include_directories(image_publisher_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(image_publisher_node PRIVATE
    vision_logger
    vision_rpc
    ${OpenCV_LIBS}
)

if(rclcpp_FOUND)
    target_link_libraries(image_publisher_node PRIVATE vision_interfaces__rosidl_typesupport_cpp)
endif()

install(TARGETS image_publisher_node RUNTIME DESTINATION image_publisher_node)

# 安装 image_publisher_node 所需的配置文件
install(FILES
    ${CMAKE_SOURCE_DIR}/config/image_publisher_config.xml
    ${CMAKE_SOURCE_DIR}/config/system_config.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zeromq.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_ros2.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zenoh.xml
    DESTINATION image_publisher_node
)
