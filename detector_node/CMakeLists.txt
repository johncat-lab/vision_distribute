add_executable(detector_node
    src/main.cpp
    src/opencv_template_detector.cpp
    src/conveyor_detector.cpp
    src/edge_gradient_detector.cpp
    src/yolo_detector.cpp
)

target_include_directories(detector_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(detector_node PRIVATE vision_rpc vision_core)

install(TARGETS detector_node RUNTIME DESTINATION bin)
