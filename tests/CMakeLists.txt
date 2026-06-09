# ========== 通信验证测试 ==========

# ZeroMQ 通信测试 (始终可构建，仅依赖 cppzmq)
add_executable(test_zmq_comm test_zmq_comm.cpp)
target_link_libraries(test_zmq_comm PRIVATE vision_rpc)

# ROS2 通信测试 (仅在 ROS2 可用时构建)
if(rclcpp_FOUND)
    add_executable(test_ros2_comm test_ros2_comm.cpp)
    target_link_libraries(test_ros2_comm PRIVATE vision_rpc)
endif()
