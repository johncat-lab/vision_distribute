# ============================================================
# Modbus 节点 - 使用 NodeContainer V2 框架
# ============================================================
set(LIBMODBUS_FOUND FALSE)
set(LIBMODBUS_LIBRARIES "")
set(LIBMODBUS_INCLUDE_DIRS "")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBMODBUS_PKG QUIET libmodbus)
endif()

if(LIBMODBUS_PKG_FOUND)
    find_library(MODBUS_LIB_FULL_PATH NAMES modbus PATHS ${LIBMODBUS_PKG_LIBRARY_DIRS})
    find_path(MODBUS_INCLUDE_FULL_PATH NAMES modbus/modbus.h PATHS ${LIBMODBUS_PKG_INCLUDE_DIRS})
else()
    find_library(MODBUS_LIB_FULL_PATH NAMES modbus)
    find_path(MODBUS_INCLUDE_FULL_PATH NAMES modbus/modbus.h)
endif()

if(MODBUS_LIB_FULL_PATH AND MODBUS_INCLUDE_FULL_PATH)
    set(LIBMODBUS_FOUND TRUE)
    set(LIBMODBUS_LIBRARIES "${MODBUS_LIB_FULL_PATH}")
    set(LIBMODBUS_INCLUDE_DIRS "${MODBUS_INCLUDE_FULL_PATH}")
    message(STATUS "libmodbus found: ${LIBMODBUS_LIBRARIES}")
else()
    message(STATUS "libmodbus not found, modbus_node will not be built")
    return()
endif()

if(APPLE)
    execute_process(COMMAND file "${LIBMODBUS_LIBRARIES}" OUTPUT_VARIABLE LIBMODBUS_FILE_INFO)
    if(LIBMODBUS_FILE_INFO MATCHES "arm64")
        message(STATUS "libmodbus is arm64 but build target is x86_64, modbus_node will not be built")
        return()
    endif()
endif()

add_executable(modbus_node
    main.cpp
    modbus_node.cpp
)

target_include_directories(modbus_node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBMODBUS_INCLUDE_DIRS}
)

target_link_libraries(modbus_node PRIVATE
    vision_logger
    vision_messages_proto
    vision_rpc
    vision_dag
    ${OpenCV_LIBS}
    ${LIBMODBUS_LIBRARIES}
)

if(rclcpp_FOUND)
    target_link_libraries(modbus_node PRIVATE vision_interfaces__rosidl_typesupport_cpp)
endif()

install(TARGETS modbus_node RUNTIME DESTINATION modbus_node)

install(FILES
    ${CMAKE_SOURCE_DIR}/config/modbus.xml
    ${CMAKE_SOURCE_DIR}/config/system_config.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zeromq.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_ros2.xml
    ${CMAKE_SOURCE_DIR}/config/system_config_zenoh.xml
    DESTINATION modbus_node
)