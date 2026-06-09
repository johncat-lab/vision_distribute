#pragma once
#include "rpc/publisher.h"
#include "rpc/subscriber.h"
#include "rpc/service.h"
#include <zmq.hpp>
#include <string>
#include <memory>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

// ========== Port computation helpers ==========

// Simple deterministic hash for port derivation
inline size_t simpleHash(const std::string& s) {
    size_t h = 0;
    for (char c : s) {
        h = h * 31 + static_cast<size_t>(static_cast<unsigned char>(c));
    }
    return h;
}

// Compute port for a pub/sub topic
inline uint16_t computeTopicPort(const std::string& topic, uint16_t base_port) {
    return base_port + static_cast<uint16_t>(simpleHash(topic) % 10);
}

// Compute port for a service endpoint
inline uint16_t computeServicePort(const std::string& endpoint, uint16_t base_port) {
    return base_port + 10 + static_cast<uint16_t>(simpleHash(endpoint) % 100);
}

// ========== ZeroMQ Publisher ==========

template<typename T>
class ZmqPublisher : public IPublisher<T> {
public:
    ZmqPublisher(zmq::context_t& context, const std::string& topic, uint16_t base_port)
        : socket_(context, zmq::socket_type::pub), topic_(topic) {
        uint16_t port = computeTopicPort(topic, base_port);
        std::string addr = "tcp://*:" + std::to_string(port);
        socket_.set(zmq::sockopt::linger, 0);
        try {
            socket_.bind(addr);
        } catch (const zmq::error_t& e) {
            throw std::runtime_error("ZmqPublisher bind failed on " + addr + ": " + e.what());
        }
    }

    bool publish(const T& msg) override {
        try {
            zmq::message_t topic_msg(topic_.data(), topic_.size());
            std::string serialized = msg.serialize();
            zmq::message_t payload(serialized.data(), serialized.size());
            socket_.send(topic_msg, zmq::send_flags::sndmore);
            socket_.send(payload, zmq::send_flags::none);
            return true;
        } catch (const zmq::error_t& e) {
            std::cerr << "ZMQ publish error: " << e.what() << std::endl;
            return false;
        }
    }

    std::string getTopic() const override { return topic_; }

private:
    zmq::socket_t socket_;
    std::string topic_;
};

// ========== ZeroMQ Subscriber ==========

template<typename T>
class ZmqSubscriber : public ISubscriber<T> {
public:
    ZmqSubscriber(zmq::context_t& context, const std::string& topic, uint16_t base_port)
        : socket_(context, zmq::socket_type::sub), topic_(topic), running_(false) {
        socket_.set(zmq::sockopt::subscribe, topic_);
        uint16_t port = computeTopicPort(topic, base_port);
        std::string addr = "tcp://localhost:" + std::to_string(port);
        try {
            socket_.connect(addr);
        } catch (const zmq::error_t& e) {
            throw std::runtime_error("ZmqSubscriber connect failed to " + addr + ": " + e.what());
        }
    }

    ~ZmqSubscriber() {
        stop();
    }

    bool subscribe(typename ISubscriber<T>::Callback cb) override {
        callback_ = cb;
        if (!running_.exchange(true)) {
            thread_ = std::thread(&ZmqSubscriber::receiveLoop, this);
        }
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;  // already stopped
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::string getTopic() const override { return topic_; }

private:
    void receiveLoop() {
        while (running_.load()) {
            zmq::pollitem_t items[] = {{static_cast<void*>(socket_), 0, ZMQ_POLLIN, 0}};
            try {
                zmq::poll(items, 1, std::chrono::milliseconds(100));
            } catch (const zmq::error_t&) {
                break;
            }

            if (items[0].revents & ZMQ_POLLIN) {
                try {
                    // Receive topic frame
                    zmq::message_t topic_msg;
                    if (!socket_.recv(topic_msg, zmq::recv_flags::none)) continue;

                    // Receive payload frame
                    zmq::message_t payload_msg;
                    if (!socket_.recv(payload_msg, zmq::recv_flags::none)) continue;

                    // Deserialize and call callback
                    std::string payload_str(
                        static_cast<const char*>(payload_msg.data()),
                        payload_msg.size()
                    );
                    T msg = T::deserialize(payload_str);
                    if (callback_) {
                        callback_(msg);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "ZmqSubscriber receive error: " << e.what() << std::endl;
                }
            }
        }
    }

    zmq::socket_t socket_;
    std::string topic_;
    typename ISubscriber<T>::Callback callback_;
    std::atomic<bool> running_;
    std::thread thread_;
};

// ========== ZeroMQ Service ==========

template<typename Request, typename Response>
class ZmqService : public IService<Request, Response> {
public:
    ZmqService(zmq::context_t& context, const std::string& name, uint16_t base_port)
        : context_(context), name_(name), base_port_(base_port), running_(false) {}

    ~ZmqService() {
        stop();
    }

    bool serve(const std::string& endpoint, typename IService<Request, Response>::Handler handler) override {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        handlers_[endpoint] = handler;

        // 如果服务器还没启动，在 base_port + 10 上启动单个 REP socket
        if (!running_.load()) {
            rep_socket_ = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::rep);
            rep_socket_->set(zmq::sockopt::linger, 0);
            // 使用固定端口作为服务端口（所有 endpoint 共享）
            uint16_t port = base_port_ + 10;
            std::string addr = "tcp://*:" + std::to_string(port);

            std::cerr << "[ZmqService::serve] " << name_ << " binding REP on " << addr
                      << " (endpoint=" << endpoint << ")" << std::endl;

            try {
                rep_socket_->bind(addr);
            } catch (const zmq::error_t& e) {
                std::cerr << "ZmqService bind failed on " << addr << ": " << e.what() << std::endl;
                rep_socket_.reset();
                return false;
            }

            running_.store(true);
            thread_ = std::thread(&ZmqService::serveLoop, this);
        } else {
            std::cerr << "[ZmqService::serve] " << name_
                      << " handler registered: " << endpoint << std::endl;
        }
        return true;
    }

    Response call(const std::string& endpoint, const Request& req) override {
        zmq::socket_t socket(context_, zmq::socket_type::req);
        socket.set(zmq::sockopt::linger, 0);
        // 连接到服务端的固定端口
        uint16_t port = base_port_ + 10;
        std::string addr = "tcp://localhost:" + std::to_string(port);

        std::cerr << "[ZmqService::call] " << name_ << "/" << endpoint
                  << " base_port=" << base_port_ << " port=" << port
                  << " addr=" << addr << std::endl;

        try {
            socket.connect(addr);
        } catch (const zmq::error_t& e) {
            std::cerr << "[ZmqService::call] CONNECT FAILED: " << e.what() << std::endl;
            throw std::runtime_error("ZmqService call connect failed to " + addr + ": " + e.what());
        }

        // Set send/recv timeout
        socket.set(zmq::sockopt::sndtimeo, 5000);
        socket.set(zmq::sockopt::rcvtimeo, 5000);

        // Send request
        std::string req_str = req.serialize();
        zmq::message_t request_msg(req_str.data(), req_str.size());
        try {
            auto send_result = socket.send(request_msg, zmq::send_flags::none);
            if (!send_result.has_value()) {
                throw std::runtime_error("ZmqService call send failed: " + endpoint);
            }
        } catch (const zmq::error_t& e) {
            throw std::runtime_error("ZmqService call send error: " + std::string(e.what()));
        }

        // Receive response with timeout via poll
        zmq::pollitem_t items[] = {{static_cast<void*>(socket), 0, ZMQ_POLLIN, 0}};
        try {
            zmq::poll(items, 1, std::chrono::milliseconds(5000));
        } catch (const zmq::error_t& e) {
            std::cerr << "[ZmqService::call] POLL ERROR: " << e.what() << std::endl;
            throw std::runtime_error("ZmqService call poll error: " + std::string(e.what()));
        }

        if (!(items[0].revents & ZMQ_POLLIN)) {
            std::cerr << "[ZmqService::call] TIMEOUT after 5s for endpoint=" << endpoint << std::endl;
            throw std::runtime_error("ZmqService call timeout: " + endpoint);
        }

        zmq::message_t response_msg;
        try {
            if (!socket.recv(response_msg, zmq::recv_flags::none)) {
                throw std::runtime_error("ZmqService call recv failed: " + endpoint);
            }
        } catch (const zmq::error_t& e) {
            throw std::runtime_error("ZmqService call recv error: " + std::string(e.what()));
        }

        std::string resp_str(
            static_cast<const char*>(response_msg.data()),
            response_msg.size()
        );
        return Response::deserialize(resp_str);
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;  // already stopped
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        rep_socket_.reset();
        handlers_.clear();
    }

private:
    void serveLoop() {
        while (running_.load()) {
            zmq::pollitem_t items[] = {{static_cast<void*>(*rep_socket_), 0, ZMQ_POLLIN, 0}};
            try {
                zmq::poll(items, 1, std::chrono::milliseconds(100));
            } catch (const zmq::error_t&) {
                break;
            }

            if (items[0].revents & ZMQ_POLLIN) {
                try {
                    zmq::message_t request_msg;
                    if (!rep_socket_->recv(request_msg, zmq::recv_flags::none)) continue;

                    std::string req_str(
                        static_cast<const char*>(request_msg.data()),
                        request_msg.size()
                    );
                    Request req = Request::deserialize(req_str);

                    // 根据请求中的 endpoint 字段分发到对应 handler
                    typename IService<Request, Response>::Handler handler;
                    {
                        std::lock_guard<std::mutex> lock(handlers_mutex_);
                        auto it = handlers_.find(req.endpoint);
                        if (it != handlers_.end()) {
                            handler = it->second;
                        } else {
                            std::cerr << "[ZmqService::serveLoop] unknown endpoint: "
                                      << req.endpoint << std::endl;
                            Response resp;
                            resp.success = false;
                            resp.data = "unknown endpoint: " + req.endpoint;
                            std::string resp_str = resp.serialize();
                            zmq::message_t response_msg(resp_str.data(), resp_str.size());
                            rep_socket_->send(response_msg, zmq::send_flags::none);
                            continue;
                        }
                    }

                    Response resp = handler(req);

                    std::string resp_str = resp.serialize();
                    zmq::message_t response_msg(resp_str.data(), resp_str.size());
                    rep_socket_->send(response_msg, zmq::send_flags::none);
                } catch (const std::exception& e) {
                    std::cerr << "ZmqService serve error: " << e.what() << std::endl;
                }
            }
        }
    }

    zmq::context_t& context_;
    std::string name_;
    uint16_t base_port_;
    std::map<std::string, typename IService<Request, Response>::Handler> handlers_;
    std::mutex handlers_mutex_;
    std::unique_ptr<zmq::socket_t> rep_socket_;
    std::atomic<bool> running_;
    std::thread thread_;
};
