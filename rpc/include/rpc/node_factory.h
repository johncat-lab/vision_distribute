#pragma once
#include "rpc/types.h"
#include "rpc/publisher.h"
#include "rpc/subscriber.h"
#include "rpc/service.h"
#include <memory>

class NodeFactory {
public:
    explicit NodeFactory(const NodeConfig& config);
    ~NodeFactory();

    template<typename T>
    std::shared_ptr<IPublisher<T>> createPublisher(const std::string& topic);

    template<typename T>
    std::shared_ptr<ISubscriber<T>> createSubscriber(const std::string& topic);

    template<typename Req, typename Resp>
    std::shared_ptr<IService<Req, Resp>> createService(const std::string& name);

private:
    NodeConfig config_;
    // Backend-specific context (zmq::context_t, etc.)
    void* transport_context_ = nullptr;
};

// ========== Template implementations ==========
// Must be in header because template instantiation requires visible definitions.

#include "zeromq_backend.h"
#include "zenoh_backend.h"
#include "ros2_backend.h"
#include <zmq.hpp>
#include <stdexcept>

template<typename T>
std::shared_ptr<IPublisher<T>> NodeFactory::createPublisher(const std::string& topic) {
    switch (config_.transport) {
    case TransportType::ZEROMQ: {
        auto* ctx = static_cast<zmq::context_t*>(transport_context_);
        return std::make_shared<ZmqPublisher<T>>(*ctx, topic, config_.base_port);
    }
    case TransportType::ZENOH:
        return std::make_shared<ZenohPublisher<T>>(topic);
    case TransportType::ROS2:
        return std::make_shared<Ros2Publisher<T>>(topic);
    }
    return nullptr;
}

template<typename T>
std::shared_ptr<ISubscriber<T>> NodeFactory::createSubscriber(const std::string& topic) {
    switch (config_.transport) {
    case TransportType::ZEROMQ: {
        auto* ctx = static_cast<zmq::context_t*>(transport_context_);
        return std::make_shared<ZmqSubscriber<T>>(*ctx, topic, config_.base_port);
    }
    case TransportType::ZENOH:
        return std::make_shared<ZenohSubscriber<T>>(topic);
    case TransportType::ROS2:
        return std::make_shared<Ros2Subscriber<T>>(topic);
    }
    return nullptr;
}

template<typename Req, typename Resp>
std::shared_ptr<IService<Req, Resp>> NodeFactory::createService(const std::string& name) {
    switch (config_.transport) {
    case TransportType::ZEROMQ: {
        auto* ctx = static_cast<zmq::context_t*>(transport_context_);
        return std::make_shared<ZmqService<Req, Resp>>(*ctx, name, config_.base_port);
    }
    case TransportType::ZENOH:
        return std::make_shared<ZenohService<Req, Resp>>(name);
    case TransportType::ROS2:
        return std::make_shared<Ros2Service<Req, Resp>>(name);
    }
    return nullptr;
}
