find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(SOURCES
    main.cpp
    src/main_window.cpp
)
set(HEADERS
    include/main_window.h
)

qt_add_executable(manager ${SOURCES} ${HEADERS})
target_include_directories(manager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(manager PRIVATE
    vision_logger
    vision_rpc
    ${OpenCV_LIBS}
    Qt6::Core Qt6::Gui Qt6::Widgets
)
install(TARGETS manager RUNTIME DESTINATION manager)

# 安装 manager 所需的配置文件
install(FILES
    ${CMAKE_SOURCE_DIR}/config/system_config.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zeromq.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_ros2.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zenoh.xml
    DESTINATION manager
)
