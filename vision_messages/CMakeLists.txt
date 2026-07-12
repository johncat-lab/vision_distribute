cmake_minimum_required(VERSION 3.16)
project(vision_messages LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# ===== 查找Protobuf =====
find_package(Protobuf REQUIRED)

# Protobuf 33.5.0+ 依赖 absl，需要显式查找
find_package(absl REQUIRED)

message(STATUS "========================================")
message(STATUS "Protobuf Configuration:")
message(STATUS "  Version: ${Protobuf_VERSION}")
message(STATUS "  Include: ${Protobuf_INCLUDE_DIRS}")
message(STATUS "  Library: ${Protobuf_LIBRARIES}")
message(STATUS "  Compiler: ${Protobuf_PROTOC_EXECUTABLE}")
message(STATUS "========================================")

# ===== Proto文件列表 =====
set(PROTO_FILES
    core/frame_msg.proto
    core/service_msg.proto
    detection/detection_msg.proto
    detection/annotation_msg.proto
)

# ===== 输出目录 =====
# 输出到 build/vision_messages 而不是 build/vision_messages/proto
# 这样 include "vision_messages/proto/core/frame_msg.pb.h" 就能正确找到
set(PROTO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR}/core)
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR}/detection)

# ===== 生成C++代码 =====
set(PROTO_SRCS)
set(PROTO_HDRS)

foreach(proto_file ${PROTO_FILES})
    get_filename_component(proto_name ${proto_file} NAME_WE)
    get_filename_component(proto_dir ${proto_file} DIRECTORY)
    
    # 输出路径直接使用 proto_dir，不需要再加 proto/ 前缀
    set(output_src "${PROTO_OUTPUT_DIR}/${proto_dir}/${proto_name}.pb.cc")
    set(output_hdr "${PROTO_OUTPUT_DIR}/${proto_dir}/${proto_name}.pb.h")
    
    add_custom_command(
        OUTPUT ${output_src} ${output_hdr}
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
            --cpp_out=${PROTO_OUTPUT_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/proto/${proto_file}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proto/${proto_file}
        COMMENT "Generating C++ code from ${proto_file}"
        VERBATIM
    )
    
    list(APPEND PROTO_SRCS ${output_src})
    list(APPEND PROTO_HDRS ${output_hdr})
endforeach()

# ===== 创建Protobuf库 =====
add_library(vision_messages_proto ${PROTO_SRCS} ${PROTO_HDRS})

target_include_directories(vision_messages_proto PUBLIC
    $<BUILD_INTERFACE:${PROTO_OUTPUT_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/proto>
    $<INSTALL_INTERFACE:include>
    ${Protobuf_INCLUDE_DIRS}
)

# 使用 Protobuf 的现代 CMake target（自动包含 absl 等依赖）
if(TARGET protobuf::libprotobuf)
    target_link_libraries(vision_messages_proto PUBLIC protobuf::libprotobuf)
    message(STATUS "使用 protobuf::libprotobuf target")
    
    # 显式链接 absl（Protobuf 33.5.0+ 的依赖）
    if(TARGET absl::base)
        target_link_libraries(vision_messages_proto PUBLIC
            absl::base
            absl::log_internal_check_op
            absl::log_internal_message
            absl::log_internal_nullguard
            absl::strings
            absl::int128
            absl::raw_logging_internal
        )
        message(STATUS "已链接 absl 库")
    endif()
else()
    target_link_libraries(vision_messages_proto PUBLIC ${Protobuf_LIBRARIES})
    message(STATUS "使用 Protobuf_LIBRARIES: ${Protobuf_LIBRARIES}")
endif()

target_compile_options(vision_messages_proto PRIVATE
    -O3
    -DNDEBUG
)

# ===== 按域创建interface库 =====
add_library(vision_messages_core INTERFACE)
target_link_libraries(vision_messages_core INTERFACE vision_messages_proto)

add_library(vision_messages_detection INTERFACE)
target_link_libraries(vision_messages_detection INTERFACE vision_messages_proto)

add_library(vision_messages_camera INTERFACE)
target_link_libraries(vision_messages_camera INTERFACE vision_messages_proto)

add_library(vision_messages_communication INTERFACE)
target_link_libraries(vision_messages_communication INTERFACE vision_messages_proto)

# ===== 安装规则 =====
install(TARGETS vision_messages_proto
    EXPORT vision_messages_targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY ${PROTO_OUTPUT_DIR}/ DESTINATION include)

# ===== 打印生成的文件 =====
message(STATUS "Generated Proto Files:")
foreach(src ${PROTO_SRCS})
    message(STATUS "  SRC: ${src}")
endforeach()
foreach(hdr ${PROTO_HDRS})
    message(STATUS "  HDR: ${hdr}")
endforeach()
