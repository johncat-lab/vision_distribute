# Protobuf迁移完整实施清单

> 版本: v1.0  
> 日期: 2026-06-23  
> 状态: 所有阶段内容已准备完成，待执行

---

## 📦 Phase 1: 基础设施搭建

### 需要创建的文件

#### 1. vision_messages/proto/core/frame_msg.proto

```protobuf
syntax = "proto3";

package vision.messages.core;

option cc_generic_services = false;

/**
 * @message FrameMsg
 * @brief 相机帧消息
 * @owner 相机模块开发人员
 * @version 1.0
 * @since 2026-06-23
 * 
 * @description
 * 从相机节点接收的图像帧数据，包含相机参数和像素数据。
 */
message FrameMsg {
    int32 camera_id = 1;              // 相机ID
    int64 timestamp = 2;              // 时间戳（毫秒）
    uint32 width = 3;                 // 图像宽度
    uint32 height = 4;                // 图像高度
    uint32 pixel_type = 5;            // 像素格式
    uint32 frame_num = 6;             // 帧序号
    float exposure_time = 7;          // 曝光时间（微秒）
    float gain = 8;                   // 增益值（dB）
    bytes data = 9;                   // 像素数据（二进制）
}
```

#### 2. vision_messages/proto/core/service_msg.proto

```protobuf
syntax = "proto3";

package vision.messages.core;

option cc_generic_services = false;

/**
 * @message ServiceRequest
 * @brief 通用服务请求
 * @owner 框架维护者
 * @version 1.0
 */
message ServiceRequest {
    string endpoint = 1;              // 服务端点名
    string payload = 2;               // 请求参数
}

/**
 * @message ServiceResponse
 * @brief 通用服务响应
 * @owner 框架维护者
 * @version 1.0
 */
message ServiceResponse {
    bool success = 1;                 // 是否成功
    string data = 2;                  // 响应数据
}
```

#### 3. vision_messages/proto/detection/detection_msg.proto

```protobuf
syntax = "proto3";

package vision.messages.detection;

option cc_generic_services = false;

/**
 * @message DetectionMsg
 * @brief 检测结果消息
 * @owner 检测模块开发人员
 * @version 1.0
 */
message DetectionMsg {
    string protocol_string = 1;       // 协议字符串 (TA,x,y,a,t)
    uint32 frame_num = 2;             // 对应的帧序号
    int64 timestamp = 3;              // 检测时间戳（毫秒）
    int32 object_count = 4;           // 检测到的物体数量
}
```

#### 4. vision_messages/proto/detection/annotation_msg.proto

```protobuf
syntax = "proto3";

package vision.messages.detection;

option cc_generic_services = false;

/**
 * @message ObjectAnnotation
 * @brief 单个物体的标注信息
 * @owner 检测模块开发人员
 * @version 1.0
 */
message ObjectAnnotation {
    double x = 1;                     // 中心X坐标（亚像素精度）
    double y = 2;                     // 中心Y坐标（亚像素精度）
    double angle = 3;                 // 旋转角度（度）
    double score = 4;                 // 检测分数（0-1）
    int32 type = 5;                   // 物体类型
    int32 id = 6;                     // 物体ID（多目标区分）
}

/**
 * @message AnnotationMsg
 * @brief 标注消息（用于分布式绘制检测结果）
 * @owner 检测模块开发人员
 * @version 1.0
 */
message AnnotationMsg {
    uint32 frame_num = 1;             // 对应的帧序号
    int64 timestamp = 2;              // 时间戳（毫秒）
    uint32 template_width = 3;        // 模板宽度
    uint32 template_height = 4;       // 模板高度
    repeated ObjectAnnotation objects = 5;  // 物体标注列表
}
```

#### 5. vision_messages/CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(vision_messages LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# ===== 查找Protobuf =====
find_package(Protobuf REQUIRED)
message(STATUS "========================================")
message(STATUS "Protobuf Configuration:")
message(STATUS "  Version: ${Protobuf_VERSION}")
message(STATUS "  Include: ${Protobuf_INCLUDE_DIRS}")
message(STATUS "  Library: ${Protobuf_LIBRARIES}")
message(STATUS "  Compiler: ${Protobuf_PROTOC_EXECUTABLE}")
message(STATUS "========================================")

# ===== Proto文件列表 =====
set(PROTO_FILES
    proto/core/frame_msg.proto
    proto/core/service_msg.proto
    proto/detection/detection_msg.proto
    proto/detection/annotation_msg.proto
)

# ===== 输出目录 =====
set(PROTO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR})
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR}/core)
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR}/detection)

# ===== 生成C++代码 =====
set(PROTO_SRCS)
set(PROTO_HDRS)

foreach(proto_file ${PROTO_FILES})
    get_filename_component(proto_name ${proto_file} NAME_WE)
    get_filename_component(proto_dir ${proto_file} DIRECTORY)
    
    set(output_src "${PROTO_OUTPUT_DIR}/${proto_dir}/${proto_name}.pb.cc")
    set(output_hdr "${PROTO_OUTPUT_DIR}/${proto_dir}/${proto_name}.pb.h")
    
    add_custom_command(
        OUTPUT ${output_src} ${output_hdr}
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
            --cpp_out=${PROTO_OUTPUT_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/${proto_file}
        DEPENDS ${proto_file}
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

target_link_libraries(vision_messages_proto PUBLIC
    ${Protobuf_LIBRARIES}
)

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
```

#### 6. 根CMakeLists.txt修改

在 `/Users/johncat/workspace/工业机器人/上下料/汇川scara/vision_distribute/CMakeLists.txt` 中：

**位置**: 在 `add_subdirectory(logger)` 之后添加

```cmake
# vision_messages 必须在 rpc 之前处理
add_subdirectory(vision_messages)
```

**修改后的片段**:
```cmake
# ========== 添加子目录 ==========
add_subdirectory(logger)

# vision_messages 必须在 rpc 之前处理
add_subdirectory(vision_messages)

# vision_interfaces 必须在 rpc 之前处理，因为 rpc 需要链接其生成的 target
if(rclcpp_FOUND)
    add_subdirectory(vision_interfaces)
endif()

add_subdirectory(rpc)
add_subdirectory(dag)
```

#### 7. vision_messages/MESSAGE_REGISTRY.md

```markdown
# Vision Messages Registry

> 最后更新: 2026-06-23

## Core Messages

| 消息类型 | 文件 | 维护者 | 版本 | 依赖方 |
|---------|------|-------|------|--------|
| FrameMsg | core/frame_msg.proto | 相机模块 | 1.0 | detector_node, manager |
| ServiceRequest | core/service_msg.proto | 框架团队 | 1.0 | 所有节点 |
| ServiceResponse | core/service_msg.proto | 框架团队 | 1.0 | 所有节点 |

## Detection Messages

| 消息类型 | 文件 | 维护者 | 版本 | 依赖方 |
|---------|------|-------|------|--------|
| DetectionMsg | detection/detection_msg.proto | 检测模块 | 1.0 | comm_node, manager |
| AnnotationMsg | detection/annotation_msg.proto | 检测模块 | 1.0 | manager |
| ObjectAnnotation | detection/annotation_msg.proto | 检测模块 | 1.0 | (嵌套消息) |

## Camera Messages (预留)

| 消息类型 | 文件 | 维护者 | 版本 | 依赖方 |
|---------|------|-------|------|--------|
| (待添加) | camera/*.proto | 相机模块 | - | - |

## Communication Messages (预留)

| 消息类型 | 文件 | 维护者 | 版本 | 依赖方 |
|---------|------|-------|------|--------|
| (待添加) | communication/*.proto | 通信模块 | - | - |

---

## 修改流程

1. 评估影响范围（查看依赖方）
2. 向后兼容性检查
3. 编写单元测试
4. 集成测试
5. 更新本文档
6. Code Review
7. 合并到主分支
```

---

## 🔄 Phase 2: 双序列化共存

### 需要修改的文件

#### 1. rpc/include/rpc/message_types.h

**修改策略**: 添加Protobuf支持，保留手写序列化

**关键修改点**:

```cpp
#pragma once

// 新增：包含Protobuf生成的头文件
#include "vision_messages/proto/core/frame_msg.pb.h"
#include "vision_messages/proto/detection/detection_msg.pb.h"
#include "vision_messages/proto/detection/annotation_msg.pb.h"
#include "vision_messages/proto/core/service_msg.pb.h"

#include <string>
#include <cstdint>
#include <vector>
#include <cstring>

// ========== 帧消息 ==========
struct FrameMsg {
    // ===== 原有字段（保持向后兼容） =====
    int camera_id = 0;
    int64_t timestamp = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t pixel_type = 0;
    uint32_t frame_num = 0;
    float exposure_time = 0;
    float gain = 0;
    std::vector<uint8_t> data;
    
    // ===== Protobuf对象（新增） =====
    vision::messages::core::FrameMsg proto_msg;
    
    // ===== 双序列化支持 =====
    static constexpr size_t HEADER_SIZE = 48;
    static constexpr uint32_t PROTO_MAGIC = 0x50524F54;  // "PROT"
    
    std::string serialize() const {
        // 如果proto_msg有数据，优先使用Protobuf
        if (proto_msg.ByteSizeLong() > 0) {
            std::string serialized;
            proto_msg.SerializeToString(&serialized);
            
            // 添加魔法头标识
            std::string result;
            result.resize(4 + serialized.size());
            uint32_t magic = PROTO_MAGIC;
            std::memcpy(&result[0], &magic, 4);
            std::memcpy(&result[4], serialized.data(), serialized.size());
            return result;
        }
        
        // 回退到手写序列化
        return serialize_legacy();
    }
    
    static FrameMsg deserialize(const std::string& buffer) {
        FrameMsg msg;
        
        // 检查是否是Protobuf格式
        if (buffer.size() >= 4) {
            uint32_t magic = 0;
            std::memcpy(&magic, buffer.data(), 4);
            
            if (magic == PROTO_MAGIC) {
                // Protobuf格式
                std::string proto_data(buffer.data() + 4, buffer.size() - 4);
                if (msg.proto_msg.ParseFromString(proto_data)) {
                    // 同步到结构体字段
                    msg.camera_id = msg.proto_msg.camera_id();
                    msg.timestamp = msg.proto_msg.timestamp();
                    msg.width = msg.proto_msg.width();
                    msg.height = msg.proto_msg.height();
                    msg.pixel_type = msg.proto_msg.pixel_type();
                    msg.frame_num = msg.proto_msg.frame_num();
                    msg.exposure_time = msg.proto_msg.exposure_time();
                    msg.gain = msg.proto_msg.gain();
                    
                    const std::string& data = msg.proto_msg.data();
                    if (!data.empty()) {
                        msg.data.assign(data.begin(), data.end());
                    }
                    return msg;
                }
            }
        }
        
        // 回退到手写反序列化
        return deserialize_legacy(buffer);
    }
    
private:
    // 原有手写序列化方法（保留）
    std::string serialize_legacy() const { /* ... 原有代码 ... */ }
    static FrameMsg deserialize_legacy(const std::string& buffer) { /* ... 原有代码 ... */ }
};
```

**注意**: DetectionMsg、AnnotationMsg、ServiceRequest、ServiceResponse采用相同的修改模式。

#### 2. camera_node/camera_node.cpp

**修改策略**: 同时填充proto_msg和原有字段

```cpp
void CameraNode::tick(std::atomic<bool>& running) {
    camera_.setImageCallback([this](const FrameInfo& info) {
        FrameMsg msg;
        
        // 填充Protobuf对象
        msg.proto_msg.set_camera_id(cam_cfg_.camera_index);
        msg.proto_msg.set_timestamp(current_time_ms());
        msg.proto_msg.set_width(info.width);
        msg.proto_msg.set_height(info.height);
        msg.proto_msg.set_pixel_type(info.pixelType);
        msg.proto_msg.set_frame_num(info.frameNum);
        msg.proto_msg.set_exposure_time(info.exposureTime);
        msg.proto_msg.set_gain(info.gain);
        if (info.data && info.dataLen > 0) {
            msg.proto_msg.set_data(std::string(
                reinterpret_cast<const char*>(info.data),
                info.dataLen
            ));
        }
        
        // 填充原有字段（向后兼容）
        msg.camera_id = cam_cfg_.camera_index;
        msg.timestamp = msg.proto_msg.timestamp();
        msg.width = msg.proto_msg.width();
        msg.height = msg.proto_msg.height();
        msg.pixel_type = msg.proto_msg.pixel_type();
        msg.frame_num = msg.proto_msg.frame_num();
        msg.exposure_time = msg.proto_msg.exposure_time();
        msg.gain = msg.proto_msg.gain();
        if (info.data && info.dataLen > 0) {
            msg.data.assign(info.data, info.data + info.dataLen);
        }
        
        if (frame_pub_) {
            frame_pub_->publish(msg);  // 自动使用Protobuf序列化
        }
    });
}
```

#### 3. detector_node/detector_node.cpp

**修改策略**: 使用proto_msg处理消息

```cpp
void DetectorNode::processFrame(const FrameMsg& frame) {
    // frame已经是Protobuf格式（带魔法头）
    // FrameMsg::deserialize()自动处理
    
    // 处理帧...
    DetectionMsg detection;
    detection.proto_msg.set_frame_num(frame.proto_msg.frame_num());
    detection.proto_msg.set_timestamp(current_time_ms());
    detection.proto_msg.set_protocol_string(result_string);
    detection.proto_msg.set_object_count(objects.size());
    
    // 填充原有字段
    detection.frame_num = detection.proto_msg.frame_num();
    // ...
    
    detection_pub_->publish(detection);
}
```

#### 4. comm_node/comm_node.cpp

**修改策略**: 使用proto_msg解析消息

```cpp
void CommNode::tick(std::atomic<bool>& running) {
    auto detection = detection_sub_->receive();
    
    // 自动反序列化（支持双格式）
    DetectionMsg msg;
    // FrameMsg::deserialize()会自动检测魔法头
    
    LOG_INFO("Detection: frame=%d, objects=%d, protocol=%s",
             msg.proto_msg.frame_num(), 
             msg.proto_msg.object_count(), 
             msg.proto_msg.protocol_string().c_str());
    
    send_to_tcp(msg.proto_msg.protocol_string());
}
```

---

## 🚀 Phase 3: 完全切换

### 需要删除/修改的内容

#### 1. 删除message_types.h中的手写序列化代码

**保留**:
- 结构体定义（可选，如果节点已完全迁移到Protobuf，可以删除）
- 常量定义（如HEADER_SIZE）

**删除**:
- `serialize()` 方法
- `deserialize()` 方法
- `serialize_legacy()` 方法
- `deserialize_legacy()` 方法
- PROTO_MAGIC常量

#### 2. 更新所有节点代码

**最终版本示例** (camera_node.cpp):

```cpp
#include "vision_messages/proto/core/frame_msg.pb.h"

void CameraNode::tick(std::atomic<bool>& running) {
    camera_.setImageCallback([this](const FrameInfo& info) {
        vision::messages::core::FrameMsg msg;
        msg.set_camera_id(cam_cfg_.camera_index);
        msg.set_timestamp(current_time_ms());
        msg.set_width(info.width);
        msg.set_height(info.height);
        msg.set_pixel_type(info.pixelType);
        msg.set_frame_num(info.frameNum);
        msg.set_exposure_time(info.exposureTime);
        msg.set_gain(info.gain);
        
        if (info.data && info.dataLen > 0) {
            msg.set_data(std::string(
                reinterpret_cast<const char*>(info.data),
                info.dataLen
            ));
        }
        
        std::string serialized;
        msg.SerializeToString(&serialized);
        
        if (frame_pub_) {
            frame_pub_->publish(serialized);
        }
    });
}
```

---

## 📝 编译验证步骤

### Phase 1验证

```bash
# 1. 安装Protobuf（如果未安装）
sudo apt install protobuf-compiler libprotobuf-dev  # Ubuntu
# 或
brew install protobuf  # macOS

# 2. 配置CMake
cd build
cmake ..

# 3. 编译vision_messages模块
make vision_messages_proto

# 4. 验证生成的文件
ls -lh vision_messages/proto/core/frame_msg.pb.h
ls -lh vision_messages/proto/core/frame_msg.pb.cc
ls -lh vision_messages/proto/detection/detection_msg.pb.h
# ... 应该看到所有.pb.h和.pb.cc文件

# 5. 编译整个项目
make -j$(nproc)

# 6. 验证不影响现有代码
./build/camera_node/camera_node_v2 --describe
./build/detector_node/detector_node_v2 --describe
./build/comm_node/comm_node_v2 --describe
```

### Phase 2验证

```bash
# 1. 编译所有节点
make -j$(nproc)

# 2. 启动完整pipeline
./build/camera_node/camera_node_v2 --config camera_config.xml &
./build/detector_node/detector_node_v2 --config detector.xml &
./build/comm_node/comm_node_v2 --config communication.xml &

# 3. 验证数据流正常
# 检查日志输出，确认消息正常传递

# 4. 停止节点
killall camera_node_v2 detector_node_v2 comm_node_v2
```

### Phase 3验证

```bash
# 1. 编译所有节点
make -j$(nproc)

# 2. 运行单元测试
ctest --output-on-failure

# 3. 运行性能测试
./build/vision_messages/tests/test_performance

# 4. 验证性能达标
# 序列化时间应该 < 20μs/次

# 5. 启动完整pipeline验证
# 同Phase 2
```

---

## ✅ 完成检查清单

### Phase 1检查清单

- [ ] Protobuf已安装（protoc --version）
- [ ] vision_messages目录结构已创建
- [ ] 所有.proto文件已创建
- [ ] vision_messages/CMakeLists.txt已创建
- [ ] 根CMakeLists.txt已修改
- [ ] MESSAGE_REGISTRY.md已创建
- [ ] 编译成功（make vision_messages_proto）
- [ ] 生成的.pb.h和.pb.cc文件存在
- [ ] 现有代码编译不受影响

### Phase 2检查清单

- [ ] message_types.h已修改（支持双序列化）
- [ ] CameraNode已更新
- [ ] DetectorNode已更新
- [ ] CommNode已更新
- [ ] 单元测试通过
- [ ] 集成测试通过
- [ ] 新旧消息格式可互操作

### Phase 3检查清单

- [ ] 所有节点已直接使用Protobuf对象
- [ ] 手写序列化代码已删除
- [ ] message_types.h已清理
- [ ] 编译无警告
- [ ] 所有测试通过
- [ ] 性能测试通过（< 20μs/次）
- [ ] MESSAGE_REGISTRY.md已更新

---

*清单版本: v1.0*  
*最后更新: 2026-06-23*  
*状态: 所有内容已准备完成，待执行*
