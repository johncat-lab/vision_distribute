#ifndef RPC_MESSAGE_TYPES_H
#define RPC_MESSAGE_TYPES_H

#include <string>
#include <cstdint>
#include <vector>
#include <cstring>

// ========== 帧消息 ==========
// 从相机节点接收的图像帧数据
// 二进制序列化格式 (48字节头部 + 像素数据):
// [4B: camera_id][8B: timestamp][2B: width][2B: height][4B: pixel_type]
// [4B: frame_num][4B: exposure_time][4B: gain][8B: data_size][data_size B: pixel data]
struct FrameMsg {
    int camera_id = 0;
    int64_t timestamp = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t pixel_type = 0;
    uint32_t frame_num = 0;
    float exposure_time = 0;
    float gain = 0;
    std::vector<uint8_t> data;   // 像素数据

    static constexpr size_t HEADER_SIZE = 48;

    std::string serialize() const {
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

    static FrameMsg deserialize(const std::string& buffer) {
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

// ========== 检测结果消息 ==========
// 检测器输出，包含协议字符串
// 二进制序列化格式:
// [4B: frame_num][8B: timestamp][4B: object_count][4B: string_length][string_length B: protocol_string]
struct DetectionMsg {
    std::string protocol_string;   // 协议字符串 (TA,x,y,a,t,...; 或 NG)
    uint32_t frame_num = 0;        // 对应的帧序号
    int64_t timestamp = 0;         // 检测时间戳 (毫秒)
    int32_t object_count = 0;      // 检测到的物体数量

    static constexpr size_t HEADER_SIZE = 20;

    std::string serialize() const {
        uint32_t str_len = static_cast<uint32_t>(protocol_string.size());
        std::string buffer;
        buffer.resize(HEADER_SIZE + str_len);

        char* ptr = &buffer[0];
        std::memcpy(ptr, &frame_num, 4);     ptr += 4;
        std::memcpy(ptr, &timestamp, 8);     ptr += 8;
        std::memcpy(ptr, &object_count, 4);  ptr += 4;
        std::memcpy(ptr, &str_len, 4);       ptr += 4;
        if (str_len > 0) {
            std::memcpy(ptr, protocol_string.data(), str_len);
        }
        return buffer;
    }

    static DetectionMsg deserialize(const std::string& buffer) {
        DetectionMsg msg;
        if (buffer.size() < HEADER_SIZE) return msg;

        const char* ptr = buffer.data();
        uint32_t str_len = 0;
        std::memcpy(&msg.frame_num, ptr, 4);    ptr += 4;
        std::memcpy(&msg.timestamp, ptr, 8);     ptr += 8;
        std::memcpy(&msg.object_count, ptr, 4);  ptr += 4;
        std::memcpy(&str_len, ptr, 4);           ptr += 4;
        if (str_len > 0 && buffer.size() >= HEADER_SIZE + str_len) {
            msg.protocol_string.assign(ptr, str_len);
        }
        return msg;
    }
};

// ========== 服务请求 ==========
// 二进制序列化格式:
// [4B: endpoint_length][endpoint_length B: endpoint][4B: payload_length][payload_length B: payload]
struct ServiceRequest {
    std::string endpoint;   // 请求的服务端点名
    std::string payload;    // 请求参数

    std::string serialize() const {
        uint32_t ep_len = static_cast<uint32_t>(endpoint.size());
        uint32_t payload_len = static_cast<uint32_t>(payload.size());
        std::string buffer;
        buffer.resize(4 + ep_len + 4 + payload_len);

        char* ptr = &buffer[0];
        std::memcpy(ptr, &ep_len, 4);          ptr += 4;
        if (ep_len > 0) {
            std::memcpy(ptr, endpoint.data(), ep_len);
            ptr += ep_len;
        }
        std::memcpy(ptr, &payload_len, 4);     ptr += 4;
        if (payload_len > 0) {
            std::memcpy(ptr, payload.data(), payload_len);
        }
        return buffer;
    }

    static ServiceRequest deserialize(const std::string& buffer) {
        ServiceRequest req;
        if (buffer.size() < 4) return req;

        const char* ptr = buffer.data();
        uint32_t ep_len = 0;
        std::memcpy(&ep_len, ptr, 4);          ptr += 4;
        if (ep_len > 0 && buffer.size() >= 4 + ep_len + 4) {
            req.endpoint.assign(ptr, ep_len);
            ptr += ep_len;
        } else {
            return req;
        }
        uint32_t payload_len = 0;
        std::memcpy(&payload_len, ptr, 4);     ptr += 4;
        if (payload_len > 0 && buffer.size() >= 4 + ep_len + 4 + payload_len) {
            req.payload.assign(ptr, payload_len);
        }
        return req;
    }
};

// ========== 标注消息 ==========
// 用于分布式绘制检测结果，包含绘制所需的完整信息
// 二进制序列化格式:
// [4B: frame_num][8B: timestamp][4B: template_width][4B: template_height][4B: object_count]
// [每个物体: 8B:x][8B:y][8B:angle][8B:score][4B:type][4B:id]
struct AnnotationMsg {
    // ===== 帧信息 =====
    uint32_t frame_num = 0;        // 对应的帧序号
    int64_t timestamp = 0;         // 时间戳 (毫秒)
    
    // ===== 模板信息 (用于绘制) =====
    uint32_t template_width = 0;   // 模板宽度
    uint32_t template_height = 0;  // 模板高度
    
    // ===== 物体标注 =====
    struct ObjectAnnotation {
        double x = 0.0;            // 中心X坐标 (亚像素精度)
        double y = 0.0;            // 中心Y坐标 (亚像素精度)
        double angle = 0.0;        // 旋转角度 (度)
        double score = 0.0;        // 检测分数 (0-1)
        int32_t type = 0;          // 物体类型
        int32_t id = 0;            // 物体ID (用于多目标区分)
        
        std::string serialize() const {
            std::string buffer;
            buffer.resize(40);
            char* ptr = &buffer[0];
            std::memcpy(ptr, &x, 8); ptr += 8;
            std::memcpy(ptr, &y, 8); ptr += 8;
            std::memcpy(ptr, &angle, 8); ptr += 8;
            std::memcpy(ptr, &score, 8); ptr += 8;
            std::memcpy(ptr, &type, 4); ptr += 4;
            std::memcpy(ptr, &id, 4);
            return buffer;
        }
        
        static ObjectAnnotation deserialize(const std::string& data) {
            ObjectAnnotation obj;
            if (data.size() < 40) return obj;
            const char* ptr = data.data();
            std::memcpy(&obj.x, ptr, 8); ptr += 8;
            std::memcpy(&obj.y, ptr, 8); ptr += 8;
            std::memcpy(&obj.angle, ptr, 8); ptr += 8;
            std::memcpy(&obj.score, ptr, 8); ptr += 8;
            std::memcpy(&obj.type, ptr, 4); ptr += 4;
            std::memcpy(&obj.id, ptr, 4);
            return obj;
        }
    };
    
    std::vector<ObjectAnnotation> objects;
    
    // ===== 序列化 =====
    static constexpr size_t HEADER_SIZE = 28;
    
    std::string serialize() const {
        uint32_t obj_count = static_cast<uint32_t>(objects.size());
        size_t obj_data_size = obj_count * 40;
        std::string buffer;
        buffer.resize(HEADER_SIZE + obj_data_size);
        
        char* ptr = &buffer[0];
        std::memcpy(ptr, &frame_num, 4); ptr += 4;
        std::memcpy(ptr, &timestamp, 8); ptr += 8;
        std::memcpy(ptr, &template_width, 4); ptr += 4;
        std::memcpy(ptr, &template_height, 4); ptr += 4;
        std::memcpy(ptr, &obj_count, 4); ptr += 4;
        
        for (const auto& obj : objects) {
            std::string obj_data = obj.serialize();
            std::memcpy(ptr, obj_data.data(), 40);
            ptr += 40;
        }
        return buffer;
    }
    
    static AnnotationMsg deserialize(const std::string& buffer) {
        AnnotationMsg msg;
        if (buffer.size() < HEADER_SIZE) return msg;
        
        const char* ptr = buffer.data();
        std::memcpy(&msg.frame_num, ptr, 4); ptr += 4;
        std::memcpy(&msg.timestamp, ptr, 8); ptr += 8;
        std::memcpy(&msg.template_width, ptr, 4); ptr += 4;
        std::memcpy(&msg.template_height, ptr, 4); ptr += 4;
        
        uint32_t obj_count = 0;
        std::memcpy(&obj_count, ptr, 4); ptr += 4;
        
        size_t expected_size = HEADER_SIZE + obj_count * 40;
        if (buffer.size() < expected_size) return msg;
        
        for (uint32_t i = 0; i < obj_count; ++i) {
            std::string obj_data(ptr, 40);
            msg.objects.push_back(ObjectAnnotation::deserialize(obj_data));
            ptr += 40;
        }
        return msg;
    }
};

// ========== 服务响应 ==========
// 二进制序列化格式:
// [1B: success flag][4B: data_length][data_length B: data]
struct ServiceResponse {
    bool success = false;
    std::string data;       // 响应数据

    std::string serialize() const {
        uint8_t success_byte = success ? 1 : 0;
        uint32_t data_len = static_cast<uint32_t>(data.size());
        std::string buffer;
        buffer.resize(1 + 4 + data_len);

        char* ptr = &buffer[0];
        std::memcpy(ptr, &success_byte, 1);    ptr += 1;
        std::memcpy(ptr, &data_len, 4);        ptr += 4;
        if (data_len > 0) {
            std::memcpy(ptr, data.data(), data_len);
        }
        return buffer;
    }

    static ServiceResponse deserialize(const std::string& buffer) {
        ServiceResponse resp;
        if (buffer.size() < 5) return resp;

        const char* ptr = buffer.data();
        uint8_t success_byte = 0;
        std::memcpy(&success_byte, ptr, 1);    ptr += 1;
        resp.success = (success_byte != 0);

        uint32_t data_len = 0;
        std::memcpy(&data_len, ptr, 4);        ptr += 4;
        if (data_len > 0 && buffer.size() >= 5 + data_len) {
            resp.data.assign(ptr, data_len);
        }
        return resp;
    }
};

#endif // RPC_MESSAGE_TYPES_H
