#include "rpc/config_loader.h"
#include <opencv2/core.hpp>
#include <iostream>
#include <stdexcept>

NodeConfig ConfigLoader::loadSystemConfig(const std::string& xml_path) {
    NodeConfig config;

    cv::FileStorage fs(xml_path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        throw std::runtime_error("无法打开配置文件: " + xml_path);
    }

    // 读取传输类型
    std::string transport_str;
    fs["transport"] >> transport_str;
    if (transport_str == "zeromq" || transport_str == "zmq") {
        config.transport = TransportType::ZEROMQ;
    } else if (transport_str == "zenoh") {
        config.transport = TransportType::ZENOH;
    } else if (transport_str == "ros2") {
        config.transport = TransportType::ROS2;
    } else {
        std::cerr << "未知传输类型: " << transport_str << "，默认使用 ZeroMQ" << std::endl;
        config.transport = TransportType::ZEROMQ;
    }

    // 读取 domain_id
    fs["domain_id"] >> config.domain_id;

    // 读取 base_port
    int base_port_val = 0;
    fs["base_port"] >> base_port_val;
    if (base_port_val > 0) {
        config.base_port = static_cast<uint16_t>(base_port_val);
    }

    // 读取 topic 配置
    std::string topic_frame, topic_detection;
    fs["topic_frame"] >> topic_frame;
    fs["topic_detection"] >> topic_detection;

    if (!topic_frame.empty()) {
        TopicConfig tc;
        tc.name = topic_frame;
        tc.type = "frame";
        config.topics["frame"] = tc;
    }

    if (!topic_detection.empty()) {
        TopicConfig tc;
        tc.name = topic_detection;
        tc.type = "detection";
        config.topics["detection"] = tc;
    }

    fs.release();

    return config;
}
