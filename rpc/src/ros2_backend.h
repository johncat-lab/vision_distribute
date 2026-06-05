#pragma once
#include "rpc/publisher.h"
#include "rpc/subscriber.h"
#include "rpc/service.h"
#include <string>
#include <stdexcept>

// ========== ROS2 Publisher 存根 ==========
template<typename T>
class Ros2Publisher : public IPublisher<T> {
public:
    Ros2Publisher(const std::string& topic) : topic_(topic) {}
    bool publish(const T& msg) override {
        throw std::runtime_error("ROS2 backend not implemented");
    }
    std::string getTopic() const override { return topic_; }
private:
    std::string topic_;
};

// ========== ROS2 Subscriber 存根 ==========
template<typename T>
class Ros2Subscriber : public ISubscriber<T> {
public:
    Ros2Subscriber(const std::string& topic) : topic_(topic) {}
    bool subscribe(typename ISubscriber<T>::Callback cb) override {
        throw std::runtime_error("ROS2 backend not implemented");
    }
    std::string getTopic() const override { return topic_; }
private:
    std::string topic_;
};

// ========== ROS2 Service 存根 ==========
template<typename Request, typename Response>
class Ros2Service : public IService<Request, Response> {
public:
    Ros2Service(const std::string& name) : name_(name) {}
    bool serve(const std::string& endpoint, typename IService<Request, Response>::Handler handler) override {
        throw std::runtime_error("ROS2 backend not implemented");
    }
    Response call(const std::string& endpoint, const Request& req) override {
        throw std::runtime_error("ROS2 backend not implemented");
    }
private:
    std::string name_;
};