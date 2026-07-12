set(DAG_SOURCES
    src/dag_scheduler.cpp
    src/service_registry.cpp
    src/dag_launcher.cpp
)

add_library(vision_dag STATIC ${DAG_SOURCES})

target_include_directories(vision_dag PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(vision_dag PUBLIC
    vision_rpc
    vision_logger
)

# tinyxml2 (优先使用 conda 环境)
find_path(TINYXML2_INCLUDE_DIR tinyxml2.h
    PATHS ${CMAKE_PREFIX_PATH}/include /usr/include /usr/local/include
    NO_DEFAULT_PATH)
find_library(TINYXML2_LIBRARY
    NAMES tinyxml2
    PATHS ${CMAKE_PREFIX_PATH}/lib /usr/lib /usr/local/lib
    NO_DEFAULT_PATH)

if(TINYXML2_INCLUDE_DIR AND TINYXML2_LIBRARY)
    target_include_directories(vision_dag PUBLIC ${TINYXML2_INCLUDE_DIR})
    target_link_libraries(vision_dag PUBLIC ${TINYXML2_LIBRARY})
    message(STATUS "tinyxml2 找到: ${TINYXML2_LIBRARY}")
else()
    message(FATAL_ERROR "未找到 tinyxml2，请安装: conda install -c conda-forge tinyxml2")
endif()

# dag_launcher 可执行文件
add_executable(dag_launcher main.cpp)
target_link_libraries(dag_launcher PRIVATE
    vision_dag
    vision_rpc
    vision_logger
)

# 安装
install(TARGETS dag_launcher RUNTIME DESTINATION dag_launcher)
install(FILES 
    ${CMAKE_SOURCE_DIR}/config/system_config.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zeromq.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_ros2.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zenoh.xml
    ${CMAKE_SOURCE_DIR}/config/dag_example.xml
    ${CMAKE_SOURCE_DIR}/config/dag_camera_detector.xml
    ${CMAKE_SOURCE_DIR}/config/dag_image_detector.xml
    DESTINATION dag_launcher 
    OPTIONAL
)
