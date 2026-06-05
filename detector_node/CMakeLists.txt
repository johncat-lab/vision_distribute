set(DETECTOR_SOURCES
    src/main.cpp
    src/opencv_template_detector.cpp
    src/conveyor_detector.cpp
    src/edge_gradient_detector.cpp
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

target_link_libraries(detector_node PRIVATE vision_rpc ${OpenCV_LIBS})

if(USE_ONNX)
    target_compile_definitions(detector_node PRIVATE HAS_ONNX)
endif()

install(TARGETS detector_node RUNTIME DESTINATION bin)
