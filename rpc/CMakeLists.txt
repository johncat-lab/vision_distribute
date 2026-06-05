set(RPC_SOURCES
    src/node_factory.cpp
    src/config_loader.cpp
    src/zeromq_backend.cpp
    src/zenoh_backend.cpp
    src/ros2_backend.cpp
)

find_package(Threads REQUIRED)

add_library(vision_rpc STATIC ${RPC_SOURCES})

target_include_directories(vision_rpc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(vision_rpc PUBLIC
    cppzmq
    ${OpenCV_LIBS}
    Threads::Threads
)

if(zenohc_FOUND)
    target_link_libraries(vision_rpc PUBLIC zenohc::shared)
    target_compile_definitions(vision_rpc PUBLIC HAS_ZENOH)
endif()

if(rclcpp_FOUND)
    find_package(std_msgs REQUIRED)
    target_link_libraries(vision_rpc PUBLIC rclcpp::rclcpp std_msgs::std_msgs__rosidl_typesupport_cpp)
    target_include_directories(vision_rpc PUBLIC ${std_msgs_INCLUDE_DIRS})
    target_compile_definitions(vision_rpc PUBLIC HAS_ROS2)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(vision_rpc PRIVATE -O2)
endif()
