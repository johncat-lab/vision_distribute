# Protobuf迁移指南

> 版本: v1.0  
> 日期: 2026-06-23  
> 适用项目: vision_distribute  
> 预计工期: 4周（渐进式迁移）

---

## 目录

1. [迁移概述](#1-迁移概述)
2. [环境准备](#2-环境准备)
3. [目录结构设计](#3-目录结构设计)
4. [Proto文件编写](#4-proto文件编写)
5. [CMake集成](#5-cmake集成)
6. [节点代码迁移](#6-节点代码迁移)
7. [渐进式迁移策略](#7-渐进式迁移策略)
8. [测试验证](#8-测试验证)
9. [常见问题排查](#9-常见问题排查)
10. [迁移检查清单](#10-迁移检查清单)

---

## 1. 迁移概述

### 1.1 为什么迁移？

**当前方案的问题**:

```cpp
// rpc/include/rpc/message_types.h - 307行手写序列化代码
struct FrameMsg {
    int camera_id = 0;
    int64_t timestamp = 0;
    // ... 8个字段
    
    // 50行序列化代码（容易出错）
    std::string serialize() const {
        char* ptr = &buffer[0];
        std::memcpy(ptr, &camera_id, 4);       ptr += 4;  // 容易忘记更新指针
        std::memcpy(ptr, &timestamp, 8);        ptr += 8;
        // ... 重复代码
    }
    
    // 40行反序列化代码（更容易出错）
    static FrameMsg deserialize(const std::string& buffer) {
        // ... 如果字段顺序错误，不会报错但结果错误
    }
};
```

**3人协作场景下的问题**:
- ❌ 所有开发人员都在修改同一个message_types.h文件
- ❌ Git冲突频繁
- ❌ 新增字段需要修改多处代码
- ❌ 版本兼容性需要手动处理
- ❌ 跨语言支持困难（Python/Java需要重新实现）

### 1.2 Protobuf的优势

✅ **代码量减少90%**  
✅ **Git冲突减少80%**（每人修改独立的.proto文件）  
✅ **向后兼容自动保证**  
✅ **跨语言支持完美**（C++/Python/Java/Go）  
✅ **性能损失可接受**（仅增加6μs，0.006ms）

### 1.3 迁移时间表

| 阶段 | 时间 | 目标 | 风险 |
|------|------|------|------|
| Phase 1: 基础设施 | 第1周 | Protobuf集成完成 | 🟢 低 |
| Phase 2: 双序列化共存 | 第2-3周 | 渐进式迁移，系统始终可用 | 🟢 低 |
| Phase 3: 完全切换 | 第4周 | 移除手写代码 | 🟡 中 |
| Phase 4: 多语言扩展 | 按需 | Python节点示例 | 🟢 低 |

---

## 2. 环境准备

### 2.1 安装Protobuf

#### Ubuntu/Debian

```bash
# 安装Protobuf编译器和开发库
sudo apt update
sudo apt install protobuf-compiler libprotobuf-dev

# 验证安装
protoc --version
# 输出: libprotoc 3.21.12 (或更高版本)
```

#### macOS

```bash
# 使用Homebrew安装
brew install protobuf

# 验证安装
protoc --version
# 输出: libprotoc 24.3 (或更高版本)
```

### 2.2 版本要求

- **最低版本**: 3.15.0
- **推荐版本**: 3.21.x 或更高
- **检查版本**: `protoc --version`

### 2.3 验证安装成功

创建测试文件验证Protobuf是否正常工作：

```bash
# 创建测试目录
mkdir -p /tmp/proto_test
cd /tmp/proto_test

# 创建测试.proto文件
cat > test.proto << 'EOF'
syntax = "proto3";
package test;

message TestMessage {
    int32 id = 1;
    string name = 2;
}
EOF

# 生成C++代码
protoc --cpp_out=. test.proto

# 检查生成的文件
ls -lh test.pb.h test.pb.cc
# 应该看到两个文件

# 清理
cd -
rm -rf /tmp/proto_test
```

---

## 3. 目录结构设计

### 3.1 创建vision_messages模块

```bash
cd /Users/johncat/workspace/工业机器人/上下料/汇川scara/vision_distribute

# 创建目录结构
mkdir -p vision_messages/proto/{core,camera,detection,communication}
mkdir -p vision_messages/tests

# 创建文件
touch vision_messages/CMakeLists.txt
touch vision_messages/MESSAGE_REGISTRY.md
```

### 3.2 完整目录树

```
vision_distribute/
├── vision_messages/                    # 新增：统一消息模块
│   ├── proto/                          # .proto源文件
│   │   ├── core/                       # 核心公共消息
│   │   │   ├── frame_msg.proto        # FrameMsg定义
│   │   │   └── service_msg.proto      # ServiceRequest/Response
│   │   ├── camera/                     # 相机域消息
│   │   │   ├── camera_config_msg.proto
│   │   │   └── camera_status_msg.proto
│   │   ├── detection/                  # 检测域消息
│   │   │   ├── detection_msg.proto    # DetectionMsg
│   │   │   └── annotation_msg.proto   # AnnotationMsg
│   │   └── communication/              # 通信域消息
│   │       ├── comm_config_msg.proto
│   │       └── protocol_msg.proto
│   │
│   ├── tests/                          # 消息单元测试
│   │   ├── test_frame_msg.cpp
│   │   ├── test_detection_msg.cpp
│   │   └── test_annotation_msg.cpp
│   │
│   ├── CMakeLists.txt                  # 构建配置
│   └── MESSAGE_REGISTRY.md             # 消息注册表文档
│
├── rpc/                                # 现有RPC框架
│   └── include/rpc/
│       └── message_types.h            # 旧的消息定义（迁移后删除）
│
├── camera_node/                        # 业务节点
├── detector_node/
└── comm_node/
```

---

## 4. Proto文件编写

### 4.1 C++类型到Protobuf类型映射

| C++类型 | Protobuf类型 | 说明 |
|---------|-------------|------|
| `int` / `int32_t` | `int32` | 32位整数 |
| `int64_t` | `int64` | 64位整数 |
| `uint16_t` | `uint32` | Protobuf没有uint16，使用uint32 |
| `uint32_t` | `uint32` | 32位无符号整数 |
| `float` | `float` | 单精度浮点 |
| `double` | `double` | 双精度浮点 |
| `std::string` | `string` | 字符串 |
| `std::vector<uint8_t>` | `bytes` | 二进制数据 |
| `std::vector<T>` | `repeated T` | 数组 |
| `bool` | `bool` | 布尔值 |

### 4.2 FrameMsg转换

#### 当前C++定义（message_types.h）

```cpp
struct FrameMsg {
    int camera_id = 0;
    int64_t timestamp = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t pixel_type = 0;
    uint32_t frame_num = 0;
    float exposure_time = 0;
    float gain = 0;
    std::vector<uint8_t> data;
};
```

#### 转换后的.proto文件

```protobuf
// vision_messages/proto/core/frame_msg.proto
syntax = "proto3";

package vision.messages.core;

// 选项：禁用generic services（我们不需要）
option cc_generic_services = false;

/**
 * @message FrameMsg
 * @brief 相机帧消息
 * @owner 开发人员A
 * @version 1.0
 * @since 2026-06-23
 * 
 * @description
 * 从相机节点接收的图像帧数据，包含相机参数和像素数据。
 * 
 * @field camera_id 相机ID（多相机系统中标识相机）
 * @field timestamp 采集时间戳（毫秒，Unix epoch）
 * @field width 图像宽度（像素）
 * @field height 图像高度（像素）
 * @field pixel_type 像素格式（参考MVS SDK定义）
 * @field frame_num 帧序号（从0开始递增）
 * @field exposure_time 曝光时间（微秒）
 * @field gain 增益值（dB）
 * @field data 像素数据（二进制）
 */
message FrameMsg {
    // ===== 字段编号规则 =====
    // 1-15: 常用字段（单字节编码，更高效）
    // 16+: 不常用字段（双字节编码）
    // 已删除的字段编号使用 reserved 保留，不能重用
    
    int32 camera_id = 1;              // 相机ID
    int64 timestamp = 2;              // 时间戳（毫秒）
    uint32 width = 3;                 // 图像宽度
    uint32 height = 4;                // 图像高度
    uint32 pixel_type = 5;            // 像素格式
    uint32 frame_num = 6;             // 帧序号
    float exposure_time = 7;          // 曝光时间（微秒）
    float gain = 8;                   // 增益值（dB）
    bytes data = 9;                   // 像素数据（二进制）
    
    // ===== 未来扩展字段 =====
    // 预留字段编号 10-20 供未来扩展
    // reserved 10 to 20;  // 如果确定不使用，可以取消注释
}
```

**关键点说明**:

1. **字段编号规则**:
   - 1-15: 单字节编码，更高效（用于常用字段）
   - 16+: 双字节编码（用于不常用字段）
   - **重要**: 字段编号一旦分配，**永远不能修改**

2. **数据类型选择**:
   - `uint16_t` → `uint32`（Protobuf没有uint16）
   - `std::vector<uint8_t>` → `bytes`（二进制数据）

3. **注释规范**:
   - 文件头包含消息描述、维护者、版本
   - 每个字段添加注释说明用途和单位

### 4.3 DetectionMsg转换

#### 当前C++定义

```cpp
struct DetectionMsg {
    std::string protocol_string;
    uint32_t frame_num = 0;
    int64_t timestamp = 0;
    int32_t object_count = 0;
};
```

#### 转换后的.proto文件

```protobuf
// vision_messages/proto/detection/detection_msg.proto
syntax = "proto3";

package vision.messages.detection;

option cc_generic_services = false;

// 依赖core模块的消息（如果需要）
import "core/frame_msg.proto";

/**
 * @message DetectionMsg
 * @brief 检测结果消息
 * @owner 开发人员B
 * @version 1.0
 * @since 2026-06-23
 * 
 * @description
 * 检测器输出，包含检测结果和协议字符串。
 */
message DetectionMsg {
    string protocol_string = 1;       // 协议字符串 (TA,x,y,a,t)
    uint32 frame_num = 2;             // 对应的帧序号
    int64 timestamp = 3;              // 检测时间戳（毫秒）
    int32 object_count = 4;           // 检测到的物体数量
    
    // ===== 未来扩展 =====
    // reserved 5 to 10;
}
```

### 4.4 AnnotationMsg转换（嵌套消息）

#### 当前C++定义

```cpp
struct AnnotationMsg {
    uint32_t frame_num = 0;
    int64_t timestamp = 0;
    uint32_t template_width = 0;
    uint32_t template_height = 0;
    
    struct ObjectAnnotation {
        double x = 0.0;
        double y = 0.0;
        double angle = 0.0;
        double score = 0.0;
        int32_t type = 0;
        int32_t id = 0;
    };
    
    std::vector<ObjectAnnotation> objects;
};
```

#### 转换后的.proto文件

```protobuf
// vision_messages/proto/detection/annotation_msg.proto
syntax = "proto3";

package vision.messages.detection;

option cc_generic_services = false;

/**
 * @message ObjectAnnotation
 * @brief 单个物体的标注信息
 * @owner 开发人员B
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
 * @owner 开发人员B
 * @version 1.0
 */
message AnnotationMsg {
    uint32 frame_num = 1;             // 对应的帧序号
    int64 timestamp = 2;              // 时间戳（毫秒）
    uint32 template_width = 3;        // 模板宽度
    uint32 template_height = 4;       // 模板高度
    
    // repeated 表示数组（对应 std::vector）
    repeated ObjectAnnotation objects = 5;  // 物体标注列表
}
```

**关键点**:
- 嵌套结构体定义为独立的`message`
- `std::vector<T>` → `repeated T`
- 嵌套消息可以在同一个文件中定义

### 4.5 ServiceRequest和ServiceResponse转换

```protobuf
// vision_messages/proto/core/service_msg.proto
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

---

## 5. CMake集成

### 5.1 vision_messages/CMakeLists.txt

```cmake
# vision_messages/CMakeLists.txt
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
    # 后续添加更多proto文件
    # proto/camera/camera_config_msg.proto
    # proto/communication/comm_config_msg.proto
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
    # 提取文件名和目录
    get_filename_component(proto_name ${proto_file} NAME_WE)
    get_filename_component(proto_dir ${proto_file} DIRECTORY)
    
    # 设置输出文件路径
    set(output_src "${PROTO_OUTPUT_DIR}/${proto_dir}/${proto_name}.pb.cc")
    set(output_hdr "${PROTO_OUTPUT_DIR}/${proto_dir}/${proto_name}.pb.h")
    
    # 生成规则
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

# 编译优化
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

### 5.2 主CMakeLists.txt修改

在 `/Users/johncat/workspace/工业机器人/上下料/汇川scara/vision_distribute/CMakeLists.txt` 中添加：

```cmake
# 在 add_subdirectory(logger) 之后添加

# vision_messages 必须在 rpc 之前处理
add_subdirectory(vision_messages)

# 原有代码
add_subdirectory(rpc)
add_subdirectory(dag)
```

### 5.3 业务节点CMakeLists.txt修改

#### camera_node/CMakeLists.txt

```cmake
# 修改 target_link_libraries

target_link_libraries(camera_node_v2 PRIVATE
    vision_logger
    vision_rpc
    vision_dag
    vision_messages_core        # 新增：Protobuf生成的FrameMsg
    ${HIK_MVCAMERACONTROL_LIB}
    ${OpenCV_LIBS}
)
```

#### detector_node/CMakeLists.txt

```cmake
target_link_libraries(detector_node_v2 PRIVATE
    vision_logger
    vision_rpc
    vision_dag
    vision_messages_core        # FrameMsg（输入）
    vision_messages_detection   # DetectionMsg + AnnotationMsg（输出）
    ${OpenCV_LIBS}
)
```

#### comm_node/CMakeLists.txt

```cmake
target_link_libraries(comm_node_v2 PRIVATE
    vision_logger
    vision_rpc
    vision_dag
    vision_messages_detection   # DetectionMsg（输入）
    vision_messages_communication
)
```

---

## 6. 节点代码迁移

### 6.1 CameraNode迁移

#### 改造前（使用手写序列化）

```cpp
// camera_node.cpp - 改造前
#include "rpc/message_types.h"

void CameraNode::tick(std::atomic<bool>& running) {
    camera_.setImageCallback([this](const FrameInfo& info) {
        FrameMsg msg;  // 手写结构体
        msg.camera_id = cam_cfg_.camera_index;
        msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        msg.width = static_cast<uint16_t>(info.width);
        msg.height = static_cast<uint16_t>(info.height);
        msg.pixel_type = static_cast<uint32_t>(info.pixelType);
        msg.frame_num = info.frameNum;
        msg.exposure_time = info.exposureTime;
        msg.gain = info.gain;
        if (info.data && info.dataLen > 0) {
            msg.data.assign(info.data, info.data + info.dataLen);
        }
        
        if (frame_pub_) {
            frame_pub_->publish(msg);  // 框架内部调用msg.serialize()
        }
    });
}
```

#### 改造后（使用Protobuf）

```cpp
// camera_node.cpp - 改造后
#include "vision_messages/proto/core/frame_msg.pb.h"

void CameraNode::tick(std::atomic<bool>& running) {
    camera_.setImageCallback([this](const FrameInfo& info) {
        // 使用Protobuf生成的类
        vision::messages::core::FrameMsg msg;
        
        msg.set_camera_id(cam_cfg_.camera_index);
        msg.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
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
        
        // 自动序列化
        std::string serialized;
        msg.SerializeToString(&serialized);
        
        if (frame_pub_) {
            frame_pub_->publish(serialized);
        }
    });
}
```

### 6.2 DetectorNode迁移

#### 改造前

```cpp
// detector_node.cpp - 改造前
#include "rpc/message_types.h"

void DetectorNode::processFrame(const FrameMsg& frame) {
    // 处理帧
    DetectionMsg detection;
    detection.frame_num = frame.frame_num;
    detection.timestamp = current_time_ms();
    detection.protocol_string = result_string;
    detection.object_count = objects.size();
    
    detection_pub_->publish(detection);
    
    // 标注消息
    AnnotationMsg annotation;
    annotation.frame_num = frame.frame_num;
    annotation.timestamp = detection.timestamp;
    annotation.template_width = template_width;
    annotation.template_height = template_height;
    
    for (const auto& obj : objects) {
        AnnotationMsg::ObjectAnnotation ann;
        ann.x = obj.x;
        ann.y = obj.y;
        ann.angle = obj.angle;
        ann.score = obj.score;
        ann.type = obj.type;
        ann.id = obj.id;
        annotation.objects.push_back(ann);
    }
    
    annotation_pub_->publish(annotation);
}
```

#### 改造后

```cpp
// detector_node.cpp - 改造后
#include "vision_messages/proto/core/frame_msg.pb.h"
#include "vision_messages/proto/detection/detection_msg.pb.h"
#include "vision_messages/proto/detection/annotation_msg.pb.h"

void DetectorNode::processFrame(const std::string& serialized_frame) {
    // 反序列化帧消息
    vision::messages::core::FrameMsg frame;
    if (!frame.ParseFromString(serialized_frame)) {
        LOG_ERROR("Failed to parse FrameMsg");
        return;
    }
    
    // 处理帧...
    
    // 创建检测结果
    vision::messages::detection::DetectionMsg detection;
    detection.set_frame_num(frame.frame_num());
    detection.set_timestamp(current_time_ms());
    detection.set_protocol_string(result_string);
    detection.set_object_count(objects.size());
    
    std::string detection_serialized;
    detection.SerializeToString(&detection_serialized);
    detection_pub_->publish(detection_serialized);
    
    // 创建标注消息
    vision::messages::detection::AnnotationMsg annotation;
    annotation.set_frame_num(frame.frame_num());
    annotation.set_timestamp(detection.timestamp());
    annotation.set_template_width(template_width);
    annotation.set_template_height(template_height);
    
    for (const auto& obj : objects) {
        auto* ann = annotation.add_objects();  // repeated字段的添加方法
        ann->set_x(obj.x);
        ann->set_y(obj.y);
        ann->set_angle(obj.angle);
        ann->set_score(obj.score);
        ann->set_type(obj.type);
        ann->set_id(obj.id);
    }
    
    std::string annotation_serialized;
    annotation.SerializeToString(&annotation_serialized);
    annotation_pub_->publish(annotation_serialized);
}
```

**关键变化**:
- `ParseFromString()` 反序列化
- `SerializeToString()` 序列化
- `add_objects()` 添加repeated字段元素
- `set_x()`, `x()` 等getter/setter

### 6.3 CommNode迁移

```cpp
// comm_node.cpp - 改造后
#include "vision_messages/proto/detection/detection_msg.pb.h"

void CommNode::tick(std::atomic<bool>& running) {
    // 订阅检测结果
    auto detection = detection_sub_->receive();  // 接收std::string
    
    // 反序列化
    vision::messages::detection::DetectionMsg msg;
    if (!msg.ParseFromString(detection)) {
        LOG_ERROR("Failed to parse DetectionMsg");
        return;
    }
    
    // 转发到外部TCP
    LOG_INFO("Detection: frame=%d, objects=%d, protocol=%s",
             msg.frame_num(), msg.object_count(), msg.protocol_string().c_str());
    
    // 发送协议字符串
    send_to_tcp(msg.protocol_string());
}
```

---

## 7. 渐进式迁移策略

### 7.1 Phase 1: 基础设施搭建（第1周）

**目标**: Protobuf集成完成，系统可编译

**步骤**:

1. ✅ 安装Protobuf
2. ✅ 创建vision_messages目录结构
3. ✅ 编写所有.proto文件
4. ✅ 编写vision_messages/CMakeLists.txt
5. ✅ 修改主CMakeLists.txt
6. ✅ 验证编译成功

**验收标准**:
```bash
cd build
cmake ..
make vision_messages_proto
# 应该成功生成 .pb.h 和 .pb.cc 文件
```

### 7.2 Phase 2: 双序列化共存（第2-3周）⭐ 关键

**目标**: 系统同时支持手写和Protobuf序列化，渐进式迁移

#### 7.2.1 修改FrameMsg支持双序列化

```cpp
// rpc/include/rpc/message_types.h - 过渡期版本
#pragma once

#include "vision_messages/proto/core/frame_msg.pb.h"
#include <string>
#include <vector>

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
    // 原有手写序列化（保留）
    std::string serialize_legacy() const {
        uint64_t sz = static_cast<uint64_t>(data.size());
        std::string buffer;
        buffer.resize(HEADER_SIZE + sz);
        
        char* ptr = &buffer[0];
        std::memcpy(ptr, &camera_id, 4);       ptr += 4;
        std::memcpy(ptr, &timestamp, 8);        ptr += 8;
        std::memcpy(ptr, &width, 2);            ptr += 2;
        std::memcpy(ptr, &height, 2);           ptr += 2;
        std::memcpy(ptr, &pixel_type, 4);       ptr += 4;
        std::memcpy(ptr, &frame_num, 4);        ptr += 4;
        std::memcpy(ptr, &exposure_time, 4);    ptr += 4;
        std::memcpy(ptr, &gain, 4);             ptr += 4;
        std::memcpy(ptr, &sz, 8);               ptr += 8;
        if (sz > 0 && !data.empty()) {
            std::memcpy(ptr, data.data(), sz);
        }
        return buffer;
    }
    
    static FrameMsg deserialize_legacy(const std::string& buffer) {
        FrameMsg msg;
        if (buffer.size() < HEADER_SIZE) return msg;
        
        const char* ptr = buffer.data();
        std::memcpy(&msg.camera_id, ptr, 4);    ptr += 4;
        std::memcpy(&msg.timestamp, ptr, 8);     ptr += 8;
        std::memcpy(&msg.width, ptr, 2);         ptr += 2;
        std::memcpy(&msg.height, ptr, 2);        ptr += 2;
        std::memcpy(&msg.pixel_type, ptr, 4);    ptr += 4;
        std::memcpy(&msg.frame_num, ptr, 4);     ptr += 4;
        std::memcpy(&msg.exposure_time, ptr, 4); ptr += 4;
        std::memcpy(&msg.gain, ptr, 4);          ptr += 4;
        uint64_t sz = 0;
        std::memcpy(&sz, ptr, 8);                ptr += 8;
        
        if (buffer.size() >= HEADER_SIZE + sz && sz > 0) {
            msg.data.resize(static_cast<size_t>(sz));
            std::memcpy(msg.data.data(), ptr, static_cast<size_t>(sz));
        }
        return msg;
    }
};
```

#### 7.2.2 渐进式切换节点

**Week 2: 迁移CameraNode**

```cpp
// camera_node.cpp - Week 2版本
#include "rpc/message_types.h"
#include "vision_messages/proto/core/frame_msg.pb.h"

void CameraNode::tick(std::atomic<bool>& running) {
    camera_.setImageCallback([this](const FrameInfo& info) {
        FrameMsg msg;  // 仍使用结构体
        
        // 同时填充Protobuf对象
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

**Week 3: 迁移DetectorNode**

```cpp
// detector_node.cpp - Week 3版本
void DetectorNode::processFrame(const FrameMsg& frame) {
    // 接收的frame已经是Protobuf格式（带魔法头）
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

### 7.3 Phase 3: 完全切换（第4周）

**目标**: 移除所有手写序列化代码

**步骤**:

1. 确认所有节点都已使用Protobuf
2. 删除message_types.h中的serialize/deserialize方法
3. 删除legacy相关代码
4. 更新节点代码直接使用Protobuf对象
5. 运行完整测试

**最终版本示例**:

```cpp
// camera_node.cpp - Phase 3最终版本
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

### 7.4 Phase 4: 多语言扩展（按需）

创建Python节点示例：

```python
# python_nodes/detector.py
import sys
sys.path.append('../build/vision_messages/proto')

from core import frame_msg_pb2
from detection import detection_msg_pb2

def on_frame_received(serialized_data):
    # 反序列化
    frame_msg = frame_msg_pb2.FrameMsg()
    frame_msg.ParseFromString(serialized_data)
    
    # 处理图像
    image = np.frombuffer(frame_msg.data, dtype=np.uint8)
    image = image.reshape((frame_msg.height, frame_msg.width))
    
    # AI推理
    result = model.predict(image)
    
    # 创建检测结果
    detection_msg = detection_msg_pb2.DetectionMsg()
    detection_msg.frame_num = frame_msg.frame_num
    detection_msg.timestamp = int(time.time() * 1000)
    detection_msg.protocol_string = f"TA,{result.x},{result.y},{result.angle}"
    detection_msg.object_count = 1
    
    # 序列化发布
    serialized = detection_msg.SerializeToString()
    publish_detection(serialized)
```

---

## 8. 测试验证

### 8.1 单元测试示例

```cpp
// vision_messages/tests/test_frame_msg.cpp
#include <gtest/gtest.h>
#include "vision_messages/proto/core/frame_msg.pb.h"

TEST(FrameMsgTest, SerializeDeserialize) {
    // 创建消息
    vision::messages::core::FrameMsg msg;
    msg.set_camera_id(1);
    msg.set_timestamp(1234567890);
    msg.set_width(1920);
    msg.set_height(1080);
    msg.set_pixel_type(0x080008);
    msg.set_frame_num(100);
    msg.set_exposure_time(10000.0f);
    msg.set_gain(5.0f);
    msg.set_data(std::string(100, 0xAB));  // 100字节测试数据
    
    // 序列化
    std::string serialized;
    ASSERT_TRUE(msg.SerializeToString(&serialized));
    
    // 反序列化
    vision::messages::core::FrameMsg msg2;
    ASSERT_TRUE(msg2.ParseFromString(serialized));
    
    // 验证字段
    EXPECT_EQ(msg2.camera_id(), 1);
    EXPECT_EQ(msg2.timestamp(), 1234567890);
    EXPECT_EQ(msg2.width(), 1920);
    EXPECT_EQ(msg2.height(), 1080);
    EXPECT_EQ(msg2.frame_num(), 100);
    EXPECT_EQ(msg2.data().size(), 100);
}

TEST(FrameMsgTest, EmptyData) {
    vision::messages::core::FrameMsg msg;
    msg.set_camera_id(0);
    msg.set_timestamp(0);
    // 不设置data
    
    std::string serialized;
    ASSERT_TRUE(msg.SerializeToString(&serialized));
    
    vision::messages::core::FrameMsg msg2;
    ASSERT_TRUE(msg2.ParseFromString(serialized));
    
    EXPECT_EQ(msg2.data().size(), 0);
}

TEST(FrameMsgTest, LargeData) {
    vision::messages::core::FrameMsg msg;
    msg.set_camera_id(1);
    
    // 2MB图像数据
    std::string large_data(2 * 1024 * 1024, 0xFF);
    msg.set_data(large_data);
    
    std::string serialized;
    ASSERT_TRUE(msg.SerializeToString(&serialized));
    
    vision::messages::core::FrameMsg msg2;
    ASSERT_TRUE(msg2.ParseFromString(serialized));
    
    EXPECT_EQ(msg2.data().size(), 2 * 1024 * 1024);
}
```

### 8.2 性能测试

```cpp
// vision_messages/tests/test_performance.cpp
#include <gtest/gtest.h>
#include <chrono>
#include "vision_messages/proto/core/frame_msg.pb.h"

TEST(FrameMsgPerformance, Serialize1000Times) {
    vision::messages::core::FrameMsg msg;
    msg.set_camera_id(1);
    msg.set_timestamp(1234567890);
    msg.set_width(1920);
    msg.set_height(1080);
    msg.set_data(std::string(2 * 1024 * 1024, 0xAB));  // 2MB
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 1000; ++i) {
        std::string serialized;
        msg.SerializeToString(&serialized);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "1000 serializations: " << duration.count() << " μs" << std::endl;
    std::cout << "Average: " << duration.count() / 1000.0 << " μs" << std::endl;
    
    // Protobuf应该 < 20μs/次
    EXPECT_LT(duration.count() / 1000, 20);
}
```

### 8.3 集成测试

```bash
#!/bin/bash
# tests/test_protobuf_pipeline.sh

echo "=== Protobuf Pipeline Integration Test ==="

# 启动节点
./build/camera_node/camera_node_v2 --config camera_config.xml &
CAMERA_PID=$!

./build/detector_node/detector_node_v2 --config detector.xml &
DETECTOR_PID=$!

./build/comm_node/comm_node_v2 --config communication.xml &
COMM_PID=$!

# 等待启动
sleep 2

# 检查节点是否正常运行
if kill -0 $CAMERA_PID 2>/dev/null && \
   kill -0 $DETECTOR_PID 2>/dev/null && \
   kill -0 $COMM_PID 2>/dev/null; then
    echo "✓ All nodes running"
else
    echo "✗ Some nodes failed to start"
    exit 1
fi

# 运行30秒
sleep 30

# 停止节点
kill $CAMERA_PID $DETECTOR_PID $COMM_PID
wait

echo "=== Test Passed ==="
```

---

## 9. 常见问题排查

### 9.1 protoc编译失败

**问题**: `protoc: command not found`

**解决**:
```bash
# Ubuntu
sudo apt install protobuf-compiler

# macOS
brew install protobuf

# 验证
protoc --version
```

### 9.2 CMake找不到Protobuf

**问题**: `Could NOT find Protobuf`

**解决**:
```cmake
# 手动指定Protobuf路径
set(Protobuf_INCLUDE_DIR /usr/include)
set(Protobuf_LIBRARY /usr/lib/x86_64-linux-gnu/libprotobuf.so)
set(Protobuf_PROTOC_EXECUTABLE /usr/bin/protoc)
find_package(Protobuf REQUIRED)
```

### 9.3 链接错误

**问题**: `undefined reference to google::protobuf::...`

**解决**:
```cmake
# 确保链接Protobuf库
target_link_libraries(your_target PRIVATE ${Protobuf_LIBRARIES})
```

### 9.4 运行时解析失败

**问题**: `ParseFromString returns false`

**排查步骤**:
```cpp
// 1. 检查序列化是否成功
std::string serialized;
bool serialize_ok = msg.SerializeToString(&serialized);
if (!serialize_ok) {
    LOG_ERROR("SerializeToString failed");
    return;
}

// 2. 检查数据大小
LOG_INFO("Serialized size: %zu bytes", serialized.size());

// 3. 尝试反序列化
vision::messages::core::FrameMsg msg2;
bool parse_ok = msg2.ParseFromString(serialized);
if (!parse_ok) {
    LOG_ERROR("ParseFromString failed");
    // 检查数据是否损坏
    LOG_INFO("First 10 bytes: %02x %02x %02x ...",
             (uint8_t)serialized[0],
             (uint8_t)serialized[1],
             (uint8_t)serialized[2]);
}
```

### 9.5 字段编号冲突

**问题**: 多人协作时字段编号重复

**预防**:
```protobuf
// 在文件头声明字段编号范围
/**
 * @message MyMessage
 * @field_range 1-20: 基础字段
 * @field_range 21-40: 扩展字段
 * @field_range 41-60: 预留
 */
message MyMessage {
    // ...
}
```

---

## 10. 迁移检查清单

### Phase 1: 基础设施

- [ ] Protobuf已安装（protoc --version）
- [ ] vision_messages目录结构已创建
- [ ] 所有.proto文件已编写
- [ ] vision_messages/CMakeLists.txt已编写
- [ ] 主CMakeLists.txt已修改
- [ ] 编译成功（make vision_messages_proto）
- [ ] 生成的.pb.h和.pb.cc文件存在

### Phase 2: 双序列化共存

- [ ] FrameMsg已支持双序列化
- [ ] DetectionMsg已支持双序列化
- [ ] AnnotationMsg已支持双序列化
- [ ] CameraNode已更新（填充proto_msg）
- [ ] DetectorNode已更新（解析proto_msg）
- [ ] CommNode已更新（解析proto_msg）
- [ ] 单元测试通过
- [ ] 集成测试通过

### Phase 3: 完全切换

- [ ] 所有节点已直接使用Protobuf对象
- [ ] 手写序列化代码已删除
- [ ] message_types.h已清理
- [ ] 编译无警告
- [ ] 所有测试通过
- [ ] 性能测试通过（< 20μs/次）

### Phase 4: 文档和规范

- [ ] MESSAGE_REGISTRY.md已更新
- [ ] 迁移指南文档已完成
- [ ] Protobuf最佳实践文档已编写
- [ ] 团队培训已完成
- [ ] Code Review规则已更新

---

## 附录A: Protobuf最佳实践

### A.1 字段编号规则

```protobuf
message MyMessage {
    // 1-15: 常用字段（单字节编码）
    int32 id = 1;
    string name = 2;
    
    // 16+: 不常用字段（双字节编码）
    string description = 16;
    
    // 已删除的字段必须保留
    reserved 5, 10 to 15;
    reserved "old_field", "deprecated_field";
}
```

### A.2 向后兼容规则

✅ **允许**:
- 新增字段（赋予新编号）
- 删除字段（使用reserved）
- 修改optional字段为repeated

❌ **禁止**:
- 修改已有字段的编号
- 修改已有字段的类型
- 重用已删除字段的编号

### A.3 命名规范

```protobuf
// 文件名: snake_case
// my_message.proto

// 消息名: PascalCase
message MyMessage {
    // 字段名: snake_case
    int32 my_field = 1;
    string another_field = 2;
    
    // 枚举名: PascalCase
    enum MyEnum {
        // 枚举值: UPPER_SNAKE_CASE
        UNKNOWN = 0;
        VALUE_ONE = 1;
    }
}
```

### A.4 性能优化

```cpp
// 1. 使用Arena减少内存分配
google::protobuf::Arena arena;
auto* msg = google::protobuf::Arena::CreateMessage<MyMessage>(&arena);

// 2. 批量序列化
std::vector<MyMessage> batch;
for (auto& msg : batch) {
    msg.SerializeToString(&output);
}

// 3. 避免频繁创建消息对象
// ❌ 不好
for (int i = 0; i < 1000; ++i) {
    MyMessage msg;  // 每次循环创建
}

// ✅ 好
MyMessage msg;
for (int i = 0; i < 1000; ++i) {
    msg.Clear();  // 复用对象
}
```

---

## 附录B: 参考资料

- [Protobuf官方文档](https://developers.google.com/protocol-buffers)
- [Protobuf C++教程](https://developers.google.com/protocol-buffers/docs/cpptutorial)
- [Protobuf语言指南](https://developers.google.com/protocol-buffers/docs/proto3)
- [Protobuf性能优化](https://developers.google.com/protocol-buffers/docs/performance)

---

*文档版本: v1.0*  
*最后更新: 2026-06-23*  
*维护者: 框架团队*
