find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(SOURCES
    main.cpp
    src/inspector_window.cpp
)
set(HEADERS
    include/inspector_window.h
)

qt_add_executable(system_inspector_gui ${SOURCES} ${HEADERS})

target_include_directories(system_inspector_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(system_inspector_gui PRIVATE
    vision_dag
    vision_rpc
    vision_logger
    ${OpenCV_LIBS}
    Qt6::Core Qt6::Gui Qt6::Widgets
)

install(TARGETS system_inspector_gui RUNTIME DESTINATION inspector)

# 安装 pipeline 配置文件供 inspector 使用
install(FILES
    ${CMAKE_SOURCE_DIR}/config/dag_camera_detector.xml
    ${CMAKE_SOURCE_DIR}/config/dag_camera_detector_comm.xml
    ${CMAKE_SOURCE_DIR}/config/dag_image_detector.xml
    ${CMAKE_SOURCE_DIR}/config/dag_example.xml
    ${CMAKE_SOURCE_DIR}/config/pipeline_detect.xml
    ${CMAKE_SOURCE_DIR}/config/pipeline_monitor.xml
    ${CMAKE_SOURCE_DIR}/config/test_pipeline.xml
    DESTINATION inspector
    OPTIONAL
)
