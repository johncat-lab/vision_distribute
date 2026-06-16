#pragma once
#include <string>
#include <vector>

/// @brief 节点自描述结构体，由 --describe 输出 JSON
struct NodeManifest {
    std::string name;        // 节点类型名（如 "camera_node"）
    std::string binary;      // 二进制文件名
    std::string version;     // 版本
    std::string config_file; // 默认配置文件名

    struct PortInfo {
        std::string port;    // 端口名（如 "frame_output"）
        std::string type;    // 消息类型（如 "FrameMsg"）
        std::string desc;    // 描述
    };

    struct ServiceInfo {
        std::string role;                     // 服务角色（如 "camera"）
        std::vector<std::string> endpoints;   // 可用端点
    };

    std::vector<PortInfo> inputs;
    std::vector<PortInfo> outputs;
    std::vector<ServiceInfo> provides_services;
    std::vector<std::string> requires_services;

    /// @brief 序列化为 JSON 字符串
    std::string toJson() const;

    /// @brief 从 JSON 字符串反序列化
    static NodeManifest fromJson(const std::string& json);
};
