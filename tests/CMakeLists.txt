# ========== 通信验证测试 ==========

# ZeroMQ 通信测试 (始终可构建，仅依赖 cppzmq)
add_executable(test_zmq_comm test_zmq_comm.cpp)
target_link_libraries(test_zmq_comm PRIVATE vision_rpc)

# Router/Dealer 并发测试
add_executable(test_router_dealer test_router_dealer.cpp)
target_link_libraries(test_router_dealer PRIVATE vision_rpc)

# 连接池优化测试
add_executable(test_connection_pool test_connection_pool.cpp)
target_link_libraries(test_connection_pool PRIVATE vision_rpc)

# Camera Node 测试 - 订阅图像并保存到文件
add_executable(test_camera_subscriber test_camera_subscriber.cpp)
target_link_libraries(test_camera_subscriber PRIVATE
    vision_rpc
    ${OpenCV_LIBS}
)

# Detection Result 测试 - 订阅检测结果并绘制标注
add_executable(test_detection_result test_detection_result.cpp)
target_link_libraries(test_detection_result PRIVATE
    vision_rpc
    vision_messages_proto
    ${OpenCV_LIBS}
)

# Template Generation 测试 - 从相机画面生成模板
add_executable(test_generate_template test_generate_template.cpp)
target_link_libraries(test_generate_template PRIVATE
    vision_rpc
    vision_messages_proto
    ${OpenCV_LIBS}
)

# ROS2 通信测试 (仅在 ROS2 可用时构建)
if(rclcpp_FOUND)
    add_executable(test_ros2_comm test_ros2_comm.cpp)
    target_link_libraries(test_ros2_comm PRIVATE vision_rpc)

    # ROS2 Service API 接口调用测试
    add_executable(test_ros2_service_api test_ros2_service_api.cpp)
    target_link_libraries(test_ros2_service_api PRIVATE vision_rpc)
endif()

install(TARGETS
    test_zmq_comm
    test_router_dealer
    test_connection_pool
    test_camera_subscriber
    test_detection_result
    test_generate_template
    RUNTIME DESTINATION tests
)

if(rclcpp_FOUND)
    install(TARGETS test_ros2_comm test_ros2_service_api RUNTIME DESTINATION tests)
endif()
