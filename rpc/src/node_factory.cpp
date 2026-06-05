#include "rpc/node_factory.h"
#include <zmq.hpp>
#include <iostream>
#include <stdexcept>

NodeFactory::NodeFactory(const NodeConfig& config)
    : config_(config) {
    switch (config_.transport) {
    case TransportType::ZEROMQ: {
        auto* ctx = new zmq::context_t(1);
        transport_context_ = ctx;
        break;
    }
    case TransportType::ZENOH:
        throw std::runtime_error("Zenoh transport not yet implemented");
    case TransportType::ROS2:
        throw std::runtime_error("ROS2 transport not yet implemented");
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
    case TransportType::ROS2:
        break;
    }
}

// Template method definitions are in node_factory.h (header-only due to C++ template requirements)
