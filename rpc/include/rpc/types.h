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
    uint16_t base_port = 5550;  // Base port for ZMQ sockets
    std::map<std::string, TopicConfig> topics;
};
