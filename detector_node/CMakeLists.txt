set(DETECTOR_SOURCES
    main.cpp
    src/opencv_template_detector.cpp
    src/conveyor_detector.cpp
    src/edge_gradient_detector.cpp
    src/ninepoint_detector.cpp
    src/calibration_core.cpp
)

# YOLO 检测器需要 ONNX Runtime
if(USE_ONNX)
    list(APPEND DETECTOR_SOURCES src/yolo_detector.cpp)
    message(STATUS "YOLO 检测器: 已启用")
else()
    message(STATUS "YOLO 检测器: 已跳过 (需要 ONNX Runtime)")
endif()

add_executable(detector_node ${DETECTOR_SOURCES})

target_include_directories(detector_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(detector_node PRIVATE vision_logger vision_rpc ${OpenCV_LIBS})

if(rclcpp_FOUND)
    target_link_libraries(detector_node PRIVATE vision_interfaces__rosidl_typesupport_cpp)
endif()

if(USE_ONNX)
    target_compile_definitions(detector_node PRIVATE HAS_ONNX)
    target_include_directories(detector_node PRIVATE ${ORT_INCLUDE_DIR})
    target_link_libraries(detector_node PRIVATE ${ORT_LIB})
endif()

install(TARGETS detector_node RUNTIME DESTINATION detector_node)

# ============================================================
# 使用 NodeContainer 框架的新检测器节点版本
# ============================================================
set(DETECTOR_V2_SOURCES
    main_new.cpp
    detector_node.cpp
    src/opencv_template_detector.cpp
    src/conveyor_detector.cpp
    src/edge_gradient_detector.cpp
    src/ninepoint_detector.cpp
    src/calibration_core.cpp
)

if(USE_ONNX)
    list(APPEND DETECTOR_V2_SOURCES src/yolo_detector.cpp)
endif()

add_executable(detector_node_v2 ${DETECTOR_V2_SOURCES})

target_include_directories(detector_node_v2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(detector_node_v2 PRIVATE vision_logger vision_rpc vision_dag ${OpenCV_LIBS})

if(rclcpp_FOUND)
    target_link_libraries(detector_node_v2 PRIVATE vision_interfaces__rosidl_typesupport_cpp)
endif()

if(USE_ONNX)
    target_compile_definitions(detector_node_v2 PRIVATE HAS_ONNX)
    target_include_directories(detector_node_v2 PRIVATE ${ORT_INCLUDE_DIR})
    target_link_libraries(detector_node_v2 PRIVATE ${ORT_LIB})
endif()

install(TARGETS detector_node_v2 RUNTIME DESTINATION detector_node)

# 安装 detector_node 所需的配置文件
install(FILES
    ${CMAKE_SOURCE_DIR}/config/detector.xml
    ${CMAKE_SOURCE_DIR}/config/system_config.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zeromq.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_ros2.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zenoh.xml
    DESTINATION detector_node
)

# 安装模版文件
install(DIRECTORY ${CMAKE_SOURCE_DIR}/template/
    DESTINATION detector_node/template
)
