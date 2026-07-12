#pragma once
#include <string>
#include <map>
#include <cstdint>

enum class TransportType {
    ZEROMQ,
    ZENOH,
    ROS2
};

struct TopicConfig {
    std::string name;
    std::string type;  // "frame", "detection", etc.
};

struct NodeConfig {
    TransportType transport = TransportType::ZEROMQ;
    std::string node_name;
    std::string domain_id;  // Zenoh: router addr, ROS2: domain id, ZMQ: empty
    uint16_t base_port = 15550;  // Base port for ZMQ sockets
    std::map<std::string, TopicConfig> topics;

    // ROS2 专用配置
    int ros2_executor_threads = 4;       // executor 线程数 (默认4)
    int service_timeout_ms = 5000;       // 服务调用超时 (毫秒)
    int service_wait_ms = 3000;          // 等待服务可用超时 (毫秒)
    int service_max_retries = 3;         // 服务调用最大重试次数
    
    // ZMQ 专用配置
    int zmq_service_workers = 4;         // ZMQ Service Worker 线程数 (Router/Dealer 模式)
};
