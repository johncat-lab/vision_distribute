#ifndef RPC_MESSAGE_TYPES_H
#define RPC_MESSAGE_TYPES_H

#include <string>
#include <cstdint>
#include <vector>

// 包含Protobuf生成的头文件
#include "core/frame_msg.pb.h"
#include "detection/detection_msg.pb.h"
#include "detection/annotation_msg.pb.h"
#include "core/service_msg.pb.h"

// ========== 消息类型别名 ==========
// 直接使用Protobuf生成的类型，零维护成本
// 修改 .proto 后只需重新 protoc，此处无需任何改动

using FrameMsg         = vision::messages::core::FrameMsg;
using DetectionMsg     = vision::messages::detection::DetectionMsg;
using AnnotationMsg    = vision::messages::detection::AnnotationMsg;
using ObjectAnnotation = vision::messages::detection::ObjectAnnotation;
using ServiceRequest   = vision::messages::core::ServiceRequest;
using ServiceResponse  = vision::messages::core::ServiceResponse;

// ========== 通用序列化/反序列化自由函数 ==========
// 一个模板覆盖所有 Protobuf 消息类型，transport 后端统一调用这些函数
// 未来新增消息类型无需修改此处

namespace vision {
namespace rpc {

/// @brief 序列化任意 Protobuf 消息为字符串
template<typename T>
inline std::string serialize(const T& msg) {
    std::string output;
    msg.SerializeToString(&output);
    return output;
}

/// @brief 从字符串反序列化为任意 Protobuf 消息
template<typename T>
inline T deserialize(const std::string& data) {
    T msg;
    msg.ParseFromString(data);
    return msg;
}

} // namespace rpc
} // namespace vision

#endif // RPC_MESSAGE_TYPES_H
