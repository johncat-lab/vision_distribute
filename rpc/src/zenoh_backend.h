#pragma once
#include "rpc/publisher.h"
#include "rpc/subscriber.h"
#include "rpc/service.h"
#include <string>
#include <stdexcept>

// ========== Zenoh Publisher 存根 ==========
template<typename T>
class ZenohPublisher : public IPublisher<T> {
public:
    ZenohPublisher(const std::string& topic) : topic_(topic) {}
    bool publish(const T& msg) override {
        throw std::runtime_error("Zenoh backend not implemented");
    }
    std::string getTopic() const override { return topic_; }
private:
    std::string topic_;
};

// ========== Zenoh Subscriber 存根 ==========
template<typename T>
class ZenohSubscriber : public ISubscriber<T> {
public:
    ZenohSubscriber(const std::string& topic) : topic_(topic) {}
    bool subscribe(typename ISubscriber<T>::Callback cb) override {
        throw std::runtime_error("Zenoh backend not implemented");
    }
    std::string getTopic() const override { return topic_; }
private:
    std::string topic_;
};

// ========== Zenoh Service 存根 ==========
template<typename Request, typename Response>
class ZenohService : public IService<Request, Response> {
public:
    ZenohService(const std::string& name) : name_(name) {}
    bool serve(const std::string& endpoint, typename IService<Request, Response>::Handler handler) override {
        throw std::runtime_error("Zenoh backend not implemented");
    }
    Response call(const std::string& endpoint, const Request& req) override {
        throw std::runtime_error("Zenoh backend not implemented");
    }
private:
    std::string name_;
};