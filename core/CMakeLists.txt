# core 库现在只保留共享接口头文件，供各节点引用
# 所有实现文件已迁移到各自的 node 目录

# 创建一个仅头文件接口库
add_library(vision_core INTERFACE)

target_include_directories(vision_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HIK_SDK_INCLUDE_DIR}
    ${ORT_INCLUDE_DIR}
)

if(USE_OPENCV)
    target_include_directories(vision_core INTERFACE ${OpenCV_INCLUDE_DIRS})
endif()

if(USE_NCNN)
    target_include_directories(vision_core INTERFACE ${NCNN_INCLUDE_DIR})
endif()

target_compile_definitions(vision_core INTERFACE HAS_HIK_CAMERA)

if(USE_OPENCV)
    target_compile_definitions(vision_core INTERFACE HAS_OPENCV)
endif()

if(USE_NCNN)
    target_compile_definitions(vision_core INTERFACE HAS_NCNN)
endif()

find_package(Threads REQUIRED)
target_link_libraries(vision_core INTERFACE
    ${HIK_MVCAMERACONTROL_LIB}
    ${ORT_LIB}
    Threads::Threads
)

if(USE_OPENCV)
    target_link_libraries(vision_core INTERFACE ${OpenCV_LIBS})
endif()

if(USE_NCNN)
    target_link_libraries(vision_core INTERFACE ${NCNN_LIB} ${OPENMP_LIB})
endif()
