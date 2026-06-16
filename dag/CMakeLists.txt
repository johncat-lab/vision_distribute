set(DAG_SOURCES
    src/dag_scheduler.cpp
    src/service_registry.cpp
)

add_library(vision_dag STATIC ${DAG_SOURCES})

target_include_directories(vision_dag PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(vision_dag PUBLIC
    vision_rpc
    vision_logger
)

# tinyxml2 (系统包)
find_package(tinyxml2 QUIET)
if(tinyxml2_FOUND)
    target_link_libraries(vision_dag PUBLIC tinyxml2::tinyxml2)
else()
    # Fallback: 手动查找
    find_path(TINYXML2_INCLUDE tinyxml2.h PATHS /usr/include /usr/local/include)
    find_library(TINYXML2_LIB tinyxml2 PATHS /usr/lib /usr/local/lib)
    if(TINYXML2_INCLUDE AND TINYXML2_LIB)
        target_include_directories(vision_dag PUBLIC ${TINYXML2_INCLUDE})
        target_link_libraries(vision_dag PUBLIC ${TINYXML2_LIB})
    else()
        message(FATAL_ERROR "未找到 tinyxml2，请安装: sudo apt install libtinyxml2-dev")
    endif()
endif()

# dag_launcher 可执行文件
add_executable(dag_launcher main.cpp)
target_link_libraries(dag_launcher PRIVATE
    vision_dag
    vision_rpc
    vision_logger
)

# 安装
install(TARGETS dag_launcher DESTINATION bins/dag_launcher)
install(FILES ${CMAKE_SOURCE_DIR}/config/system_config.xml DESTINATION bins/dag_launcher OPTIONAL)
