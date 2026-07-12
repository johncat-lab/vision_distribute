cmake_minimum_required(VERSION 3.16)
project(vision_distribute LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置安装后的 RPATH，让可执行文件自动找到共享库
# 二进制安装路径: install/bins/<nodename>/<binary>，库路径: install/lib/
# 同时添加 conda 环境路径以找到系统库（Protobuf, OpenCV 等）
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../../lib;${CMAKE_PREFIX_PATH}/lib")
    set(CMAKE_BUILD_RPATH "@loader_path/../../lib;${CMAKE_PREFIX_PATH}/lib")
else()
    set(CMAKE_INSTALL_RPATH "\$ORIGIN/../../lib;${CMAKE_PREFIX_PATH}/lib")
    set(CMAKE_BUILD_RPATH "\$ORIGIN/../../lib;${CMAKE_PREFIX_PATH}/lib")
endif()

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64")
endif()

# ========== 安装前缀默认值 ==========
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install/bins" CACHE PATH "Install prefix" FORCE)
endif()

# ========== 海康机器人 MVS SDK 配置 ==========
option(USE_HIK "Build with Hikrobot MVS camera support" ON)

if(USE_HIK)
    # 平台相关路径配置
    if(APPLE)
        # macOS 平台
        set(HIK_SDK_PATH_DEFAULT "/Library/MVS_SDK")
        set(HIK_SDK_INCLUDE_SUBDIR "Includes")
        set(HIK_SDK_LIB_SUBDIR "lib")
        set(HIK_LIB_NAME "MvCameraControl")
        message(STATUS "[海康SDK] 检测到 macOS 平台")
    elseif(UNIX)
        # Linux 平台
        set(HIK_SDK_PATH_DEFAULT "/opt/MVS")
        set(HIK_SDK_INCLUDE_SUBDIR "include")
        set(HIK_SDK_LIB_SUBDIR "lib/64")
        set(HIK_LIB_NAME "MvCameraControl")
        message(STATUS "[海康SDK] 检测到 Linux 平台")
    else()
        message(WARNING "[海康SDK] 不支持的操作系统，将跳过 camera_node")
        set(USE_HIK OFF)
    endif()

    if(USE_HIK)
        # 优先使用 build.sh 传入的 HIK_SDK_ROOT，否则使用默认路径
        if(HIK_SDK_ROOT)
            set(HIK_SDK_PATH "${HIK_SDK_ROOT}" CACHE PATH "Path to Hikrobot MVS SDK" FORCE)
            message(STATUS "[海康SDK] 使用 build.sh 传入的路径: ${HIK_SDK_ROOT}")
        else()
            set(HIK_SDK_PATH "${HIK_SDK_PATH_DEFAULT}" CACHE PATH "Path to Hikrobot MVS SDK")
        endif()
        
        # 设置包含目录和库目录
        set(HIK_SDK_INCLUDE_DIR "${HIK_SDK_PATH}/${HIK_SDK_INCLUDE_SUBDIR}")
        set(HIK_SDK_LIB_DIR "${HIK_SDK_PATH}/${HIK_SDK_LIB_SUBDIR}")

        # 查找库文件
        find_library(HIK_MVCAMERACONTROL_LIB
            NAMES ${HIK_LIB_NAME}
            PATHS ${HIK_SDK_LIB_DIR}
            NO_DEFAULT_PATH
        )

        if(NOT HIK_MVCAMERACONTROL_LIB)
            message(WARNING "[海康SDK] 未找到库文件: ${HIK_SDK_LIB_DIR}/lib${HIK_LIB_NAME}，将跳过 camera_node")
            message(WARNING "[海康SDK] 可通过 -DHIK_SDK_PATH=<path> 指定SDK路径")
            set(USE_HIK OFF)
        else()
            message(STATUS "[海康SDK] 路径: ${HIK_SDK_PATH}")
            message(STATUS "[海康SDK] 包含目录: ${HIK_SDK_INCLUDE_DIR}")
            message(STATUS "[海康SDK] 库文件: ${HIK_MVCAMERACONTROL_LIB}")
            message(STATUS "[海康SDK] 已启用")
        endif()
    endif()
endif()

# ========== ONNX Runtime 配置 (可选) ==========
option(USE_ONNX "Build with ONNX Runtime YOLO support" ON)

if(USE_ONNX)
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
        message(WARNING "未找到 ONNX Runtime 库: ${ORT_LIB_DIR}，将禁用 YOLO 检测器")
        set(USE_ONNX OFF)
    else()
        message(STATUS "ONNX Runtime 路径: ${ORT_SDK_PATH}")
        message(STATUS "ONNX Runtime 库: ${ORT_LIB}")
    endif()
endif()

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
find_package(cppzmq QUIET)
if(NOT cppzmq_FOUND)
    # Fallback: 手动查找 zmq.hpp 和 libzmq
    find_path(ZMQ_HEADER zmq.hpp PATHS /usr/include /usr/local/include)
    find_library(ZMQ_LIB zmq PATHS /usr/lib /usr/local/lib)
    if(ZMQ_HEADER AND ZMQ_LIB)
        message(STATUS "ZeroMQ: 手动找到 zmq.hpp (${ZMQ_HEADER}) + libzmq (${ZMQ_LIB})")
        add_library(cppzmq INTERFACE)
        target_include_directories(cppzmq INTERFACE ${ZMQ_HEADER})
        target_link_libraries(cppzmq INTERFACE ${ZMQ_LIB})
    else()
        message(FATAL_ERROR "未找到 ZeroMQ，请安装 libzmq3-dev 和 cppzmq-dev: sudo apt install libzmq3-dev cppzmq-dev")
    endif()
else()
    message(STATUS "ZeroMQ (cppzmq) 已找到")
endif()

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

# ========== 添加子目录 ==========
add_subdirectory(logger)

# vision_messages 必须在 rpc 之前处理，因为节点需要链接 Protobuf 消息库
add_subdirectory(vision_messages)

# vision_interfaces 必须在 rpc 之前处理，因为 rpc 需要链接其生成的 target
if(rclcpp_FOUND)
    add_subdirectory(vision_interfaces)
endif()

add_subdirectory(rpc)
add_subdirectory(dag)

if(USE_HIK)
    add_subdirectory(camera_node)
else()
    message(STATUS "camera_node 已跳过 (需要海康 MVS SDK，可通过 -DUSE_HIK=OFF 禁用)")
endif()

add_subdirectory(detector_node)
add_subdirectory(comm_node)
add_subdirectory(modbus_node)
add_subdirectory(image_publisher_node)

if(BUILD_GUI)
    add_subdirectory(manager)
    add_subdirectory(inspector)
endif()

# ========== 通信验证测试 ==========
add_subdirectory(tests)

# ========== 配置文件和模版文件由各节点 CMakeLists.txt 单独安装 ==========

# ========== Release 优化 ==========
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()