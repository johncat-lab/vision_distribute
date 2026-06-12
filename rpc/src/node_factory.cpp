#include "rpc/node_factory.h"
#include <zmq.hpp>
#include <iostream>
#include <stdexcept>

// 各后端的全局初始化函数声明
#ifdef HAS_ROS2
namespace ros2_global {
    void init(const std::string& node_name, int executor_threads);
    void shutdown();
}
#endif

#ifdef HAS_ZENOH
namespace zenoh_global {
    void init();
    void shutdown();
}
#endif

NodeFactory::NodeFactory(const NodeConfig& config)
    : config_(config) {
    switch (config_.transport) {
    case TransportType::ZEROMQ: {
        auto* ctx = new zmq::context_t(1);
        transport_context_ = ctx;
        break;
    }
    case TransportType::ZENOH:
#ifdef HAS_ZENOH
        zenoh_global::init();
#else
        throw std::runtime_error("Zenoh backend not available (rebuild with zenohc)");
#endif
        break;
    case TransportType::ROS2:
#ifdef HAS_ROS2
        ros2_global::init(config.node_name.empty() ? "vision_node" : config.node_name, config.ros2_executor_threads);
        ros2_global::start_executor();
#else
        throw std::runtime_error("ROS2 backend not available (rebuild with rclcpp)");
#endif
        break;
    }
}

NodeFactory::~NodeFactory() {
    switch (config_.transport) {
    case TransportType::ZEROMQ: {
        auto* ctx = static_cast<zmq::context_t*>(transport_context_);
        delete ctx;
        break;
    }
    case TransportType::ZENOH:
#ifdef HAS_ZENOH
        zenoh_global::shutdown();
#endif
        break;
    case TransportType::ROS2:
#ifdef HAS_ROS2
        ros2_global::shutdown();
#endif
        break;
    }
}

// Template method definitions are in node_factory.h (header-only due to C++ template requirements)
