cmake_minimum_required(VERSION 3.16)
project(vision_distribute LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64")
endif()

# ========== 安装前缀默认值 ==========
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install prefix" FORCE)
endif()

# ========== 海康机器人 MVS SDK 配置 ==========
if(UNIX AND NOT APPLE)
    set(HIK_SDK_PATH_DEFAULT "/opt/MVS")
else()
    set(HIK_SDK_PATH_DEFAULT "/Library/MVS_SDK")
endif()
set(HIK_SDK_PATH "${HIK_SDK_PATH_DEFAULT}" CACHE PATH "Path to Hikrobot MVS SDK")
if(UNIX AND NOT APPLE)
    set(HIK_SDK_INCLUDE_DIR "${HIK_SDK_PATH}/include")
else()
    set(HIK_SDK_INCLUDE_DIR "${HIK_SDK_PATH}/Includes")
endif()
if(UNIX AND NOT APPLE)
    set(HIK_SDK_LIB_DIR "${HIK_SDK_PATH}/lib/64")
else()
    set(HIK_SDK_LIB_DIR "${HIK_SDK_PATH}/lib")
endif()

find_library(HIK_MVCAMERACONTROL_LIB
    NAMES MvCameraControl
    PATHS ${HIK_SDK_LIB_DIR}
    NO_DEFAULT_PATH
)

if(NOT HIK_MVCAMERACONTROL_LIB)
    message(FATAL_ERROR "未找到海康 MVS SDK 库，请确认安装路径: ${HIK_SDK_LIB_DIR}")
endif()

message(STATUS "海康 SDK 路径: ${HIK_SDK_PATH}")
message(STATUS "海康 SDK 库: ${HIK_MVCAMERACONTROL_LIB}")

# ========== ONNX Runtime 配置 ==========
if(UNIX AND NOT APPLE)
    set(ORT_SDK_PATH_DEFAULT "${CMAKE_SOURCE_DIR}/deps/onnxruntime-linux-x64-1.17.3")
else()
    set(ORT_SDK_PATH_DEFAULT "${CMAKE_SOURCE_DIR}/deps/onnxruntime-osx-x86_64-1.17.3")
endif()
set(ORT_SDK_PATH "${ORT_SDK_PATH_DEFAULT}"
    CACHE PATH "Path to ONNX Runtime x86_64 SDK")
set(ORT_INCLUDE_DIR "${ORT_SDK_PATH}/include")
set(ORT_LIB_DIR "${ORT_SDK_PATH}/lib")

find_library(ORT_LIB
    NAMES onnxruntime
    PATHS ${ORT_LIB_DIR}
    NO_DEFAULT_PATH
)

if(NOT ORT_LIB)
    message(FATAL_ERROR "未找到 ONNX Runtime 库，请确认安装路径: ${ORT_LIB_DIR}")
endif()

message(STATUS "ONNX Runtime 路径: ${ORT_SDK_PATH}")
message(STATUS "ONNX Runtime 库: ${ORT_LIB}")

# ========== NCNN 配置 (可选) ==========
option(USE_NCNN "Build with NCNN inference backend" ON)

if(USE_NCNN)
    if(UNIX AND NOT APPLE)
        set(NCNN_SDK_PATH_DEFAULT "${CMAKE_SOURCE_DIR}/deps/ncnn-linux-x64")
    else()
        set(NCNN_SDK_PATH_DEFAULT "${CMAKE_SOURCE_DIR}/deps/ncnn-osx-x86_64")
    endif()
    set(NCNN_SDK_PATH "${NCNN_SDK_PATH_DEFAULT}"
        CACHE PATH "Path to NCNN SDK")
    set(NCNN_INCLUDE_DIR "${NCNN_SDK_PATH}/include")
    set(NCNN_LIB_DIR "${NCNN_SDK_PATH}/lib")

    find_library(NCNN_LIB
        NAMES ncnn
        PATHS ${NCNN_LIB_DIR}
             ${NCNN_LIB_DIR}/ncnn.framework
        NO_DEFAULT_PATH
    )

    if(APPLE AND NOT NCNN_LIB)
        find_library(NCNN_LIB
            NAMES ncnn
            PATHS ${NCNN_LIB_DIR}/ncnn.framework/Versions/A
            NO_DEFAULT_PATH
        )
    endif()

    if(NOT NCNN_LIB)
        message(WARNING "未找到 NCNN 库，请确认安装路径: ${NCNN_LIB_DIR}，将禁用 NCNN 支持")
        set(USE_NCNN OFF)
    else()
        if(APPLE AND IS_DIRECTORY "${NCNN_LIB_DIR}/ncnn.framework")
            set(NCNN_FRAMEWORK_DIR "${NCNN_LIB_DIR}")
            set(NCNN_USE_FRAMEWORK ON)
        endif()
        message(STATUS "NCNN 路径: ${NCNN_SDK_PATH}")
        message(STATUS "NCNN 库: ${NCNN_LIB}")
    endif()

    if(USE_NCNN AND APPLE AND IS_DIRECTORY "${NCNN_LIB_DIR}/openmp.framework")
        set(OPENMP_LIB "${NCNN_LIB_DIR}/openmp.framework/Versions/A/openmp" CACHE FILEPATH "OpenMP library from NCNN SDK")
        if(EXISTS "${OPENMP_LIB}")
            message(STATUS "NCNN OpenMP 库: ${OPENMP_LIB}")
        else()
            set(OPENMP_LIB "")
            message(STATUS "NCNN OpenMP 库未找到")
        endif()
    endif()
endif()

# ========== OpenCV 配置 ==========
option(USE_OPENCV "Build with OpenCV template detector and undistortion" ON)

if(USE_OPENCV)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs highgui calib3d)
    message(STATUS "OpenCV 版本: ${OpenCV_VERSION}")
    message(STATUS "OpenCV 库: ${OpenCV_LIBS}")

    if(APPLE AND TARGET opencv_core)
        get_target_property(_opencv_lib_path opencv_core IMPORTED_LOCATION)
        if(NOT _opencv_lib_path)
            get_target_property(_opencv_lib_path opencv_core IMPORTED_LOCATION_RELEASE)
        endif()
        if(_opencv_lib_path)
            get_filename_component(OPENCV_LIB_DIR "${_opencv_lib_path}" DIRECTORY)
            message(STATUS "OpenCV 库目录: ${OPENCV_LIB_DIR}")
        endif()
    endif()
endif()

# ========== ZeroMQ 配置 ==========
find_package(cppzmq REQUIRED)
message(STATUS "ZeroMQ (cppzmq) 已找到")

# ========== Zenoh 配置 (可选) ==========
find_package(zenohc QUIET)
if(zenohc_FOUND)
    message(STATUS "Zenoh 已找到")
else()
    message(STATUS "Zenoh 未找到，将使用存根实现")
endif()

# ========== ROS2 配置 (可选) ==========
find_package(rclcpp QUIET)
if(rclcpp_FOUND)
    message(STATUS "ROS2 (rclcpp) 已找到")
else()
    message(STATUS "ROS2 未找到，将使用存根实现")
endif()

# ========== 构建选项 ==========
option(BUILD_GUI "Build Qt6 GUI manager application" OFF)

# ========== 添加 core 子目录 ==========
add_subdirectory(core)

# ========== 添加子目录 ==========
add_subdirectory(rpc)
add_subdirectory(camera_node)
add_subdirectory(detector_node)
add_subdirectory(comm_node)

if(BUILD_GUI)
    add_subdirectory(manager)
endif()

# ========== 安装配置文件 ==========
install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/
    DESTINATION config
)

# ========== Release 优化 ==========
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()