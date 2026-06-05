add_executable(camera_node
    src/main.cpp
    src/hik_camera.cpp
)

target_include_directories(camera_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(camera_node PRIVATE
    vision_rpc
    vision_core
    ${OpenCV_LIBS}
)

install(TARGETS camera_node RUNTIME DESTINATION bin)
