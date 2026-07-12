#pragma once
#include "rpc/publisher.h"
#include "rpc/subscriber.h"
#include "rpc/service.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include <zmq.hpp>
#include <string>
#include <memory>
#include <cstring>
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

// Compute port for a pub/sub topic (base_port ~ base_port + 9)
inline uint16_t computeTopicPort(const std::string& topic, uint16_t base_port) {
    return base_port + static_cast<uint16_t>(simpleHash(topic) % 10);
}

// Compute port for a service (base_port + 10 ~ base_port + 109)
inline uint16_t computeServicePort(const std::string& service_name, uint16_t base_port) {
    return base_port + 10 + static_cast<uint16_t>(simpleHash(service_name) % 100);
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
            std::string serialized;
            // 支持 Protobuf 消息和带有 serialize() 方法的结构体
            if constexpr (std::is_same_v<T, AnnotationMsg>) {
                serialized = msg.serialize();
            } else {
                msg.SerializeToString(&serialized);
            }
            zmq::message_t payload(serialized.data(), serialized.size());
            socket_.send(topic_msg, zmq::send_flags::sndmore);
            socket_.send(payload, zmq::send_flags::none);
            return true;
        } catch (const zmq::error_t& e) {
            LOG_ERROR("ZMQ publish error: %s", e.what());
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
                    T msg;
                    // 支持 Protobuf 消息和带有 deserialize() 方法的结构体
                    if constexpr (std::is_same_v<T, AnnotationMsg>) {
                        msg = T::deserialize(payload_str);
                    } else {
                        msg.ParseFromString(payload_str);
                    }
                    if (callback_) {
                        callback_(msg);
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("ZmqSubscriber receive error: %s", e.what());
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
    ZmqService(zmq::context_t& context, const std::string& name, uint16_t base_port, int num_workers = 4)
        : context_(context), name_(name), base_port_(base_port), 
          num_workers_(num_workers), running_(false) {
        // 初始化客户端连接池 (预创建 2 个连接)
        client_pool_size_ = 2;
    }

    ~ZmqService() {
        stop();
    }

    void preconnect() override {
        // 预创建客户端连接池
        initializeClientPool();
    }

    bool serve(const std::string& endpoint, typename IService<Request, Response>::Handler handler) override {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        handlers_[endpoint] = handler;

        // 如果服务器还没启动，启动 Router/Dealer 架构
        if (!running_.load()) {
            startRouterDealer();
        } else {
            LOG_INFO("[ZmqService::serve] %s handler registered: %s", name_.c_str(), endpoint.c_str());
        }
        return true;
    }

    Response call(const std::string& endpoint, const Request& req) override {
        // 创建独立的 socket (REQ 不支持并发共享)
        auto socket = getClientSocket();
        
        // 构建请求
        std::string req_str;
        Request mutable_req = req;
        mutable_req.set_endpoint(endpoint);
        mutable_req.SerializeToString(&req_str);
        
        zmq::message_t request_msg(req_str.data(), req_str.size());
        try {
            auto send_result = socket->send(request_msg, zmq::send_flags::none);
            if (!send_result.has_value()) {
                throw std::runtime_error("ZmqService call send failed: " + endpoint);
            }
        } catch (const zmq::error_t& e) {
            throw std::runtime_error("ZmqService call send error: " + std::string(e.what()));
        }

        // 接收响应
        zmq::pollitem_t items[] = {{static_cast<void*>(*socket), 0, ZMQ_POLLIN, 0}};
        try {
            zmq::poll(items, 1, std::chrono::milliseconds(5000));
        } catch (const zmq::error_t& e) {
            LOG_ERROR("[ZmqService::call] POLL ERROR: %s", e.what());
            throw std::runtime_error("ZmqService call poll error: " + std::string(e.what()));
        }

        if (!(items[0].revents & ZMQ_POLLIN)) {
            LOG_ERROR("[ZmqService::call] TIMEOUT after 5s for endpoint=%s", endpoint.c_str());
            throw std::runtime_error("ZmqService call timeout: " + endpoint);
        }

        zmq::message_t response_msg;
        try {
            if (!socket->recv(response_msg, zmq::recv_flags::none)) {
                throw std::runtime_error("ZmqService call recv failed: " + endpoint);
            }
        } catch (const zmq::error_t& e) {
            throw std::runtime_error("ZmqService call recv error: " + std::string(e.what()));
        }

        std::string resp_str(
            static_cast<const char*>(response_msg.data()),
            response_msg.size()
        );
        Response resp;
        resp.ParseFromString(resp_str);
        return resp;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;  // already stopped
        }
        
        // 等待前端线程
        if (frontend_thread_.joinable()) {
            frontend_thread_.join();
        }
        
        // 等待所有 worker 线程
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        
        workers_.clear();
        handlers_.clear();
        
        // 清理客户端连接池
        clearClientPool();
        
        LOG_INFO("[ZmqService] %s Router/Dealer 已停止", name_.c_str());
    }

private:
    /// @brief 启动 Router/Dealer 架构
    void startRouterDealer() {
        running_.store(true);
        
        std::string workers_addr = "inproc://workers-pool-" + name_;
        
        // 先启动 Worker 线程 (它们会 connect)
        for (int i = 0; i < num_workers_; ++i) {
            workers_.emplace_back(&ZmqService::workerLoop, this, workers_addr);
        }
        
        // 等待 worker 启动
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // 再启动前端线程 (它会 bind)
        frontend_thread_ = std::thread(&ZmqService::frontendLoop, this, workers_addr);
        
        LOG_INFO("[ZmqService] Router/Dealer 模式已启动: %s, workers=%d", 
                 name_.c_str(), num_workers_);
    }
    
    /// @brief 前端循环: ROUTER socket 负责接收客户端请求并转发到 Worker
    void frontendLoop(const std::string& workers_addr) {
        zmq::socket_t router(context_, zmq::socket_type::router);
        router.set(zmq::sockopt::linger, 0);
        
        uint16_t port = computeServicePort(name_, base_port_);
        std::string addr = "tcp://*:" + std::to_string(port);
        
        try {
            router.bind(addr);
        } catch (const zmq::error_t& e) {
            LOG_ERROR("[ZmqService::frontend] ROUTER bind failed on %s: %s", 
                      addr.c_str(), e.what());
            return;
        }
        
        // 连接到内部 worker 池
        zmq::socket_t dealer(context_, zmq::socket_type::dealer);
        dealer.set(zmq::sockopt::linger, 0);
        dealer.bind(workers_addr);  // frontend bind, worker connect
        
        LOG_INFO("[ZmqService::frontend] ROUTER 前端已绑定: %s", addr.c_str());
        LOG_INFO("[ZmqService::frontend] Worker 地址: %s", workers_addr.c_str());
        
        while (running_.load()) {
            zmq::pollitem_t items[] = {
                {static_cast<void*>(router), 0, ZMQ_POLLIN, 0},
                {static_cast<void*>(dealer), 0, ZMQ_POLLIN, 0}
            };
            
            try {
                zmq::poll(items, 2, std::chrono::milliseconds(100));
            } catch (const zmq::error_t&) {
                break;
            }
            
            // 客户端请求 → 转发到 Worker
            if (items[0].revents & ZMQ_POLLIN) {
                try {
                    zmq::message_t client_id, empty, request;
                    
                    // 接收: [client_id] [] [request]
                    LOG_DEBUG("[ZmqService::frontend] 接收到客户端请求");
                    if (!router.recv(client_id, zmq::recv_flags::none)) continue;
                    if (!router.recv(empty, zmq::recv_flags::none)) continue;
                    if (!router.recv(request, zmq::recv_flags::none)) continue;
                    
                    // 转发: [client_id] [] [request]
                    LOG_DEBUG("[ZmqService::frontend] 转发请求到 Worker");
                    dealer.send(client_id, zmq::send_flags::sndmore);
                    dealer.send(empty, zmq::send_flags::sndmore);
                    dealer.send(request, zmq::send_flags::none);
                    
                } catch (const std::exception& e) {
                    LOG_ERROR("[ZmqService::frontend] 转发请求错误: %s", e.what());
                }
            }
            
            // Worker 响应 → 转发回客户端
            if (items[1].revents & ZMQ_POLLIN) {
                try {
                    zmq::message_t client_id, empty, response;
                    
                    // 接收: [client_id] [] [response]
                    if (!dealer.recv(client_id, zmq::recv_flags::none)) continue;
                    if (!dealer.recv(empty, zmq::recv_flags::none)) continue;
                    if (!dealer.recv(response, zmq::recv_flags::none)) continue;
                    
                    // 发送: [client_id] [] [response]
                    router.send(client_id, zmq::send_flags::sndmore);
                    router.send(empty, zmq::send_flags::sndmore);
                    router.send(response, zmq::send_flags::none);
                    
                } catch (const std::exception& e) {
                    LOG_ERROR("[ZmqService::frontend] 转发响应错误: %s", e.what());
                }
            }
        }
        
        LOG_INFO("[ZmqService::frontend] ROUTER 前端已停止");
    }
    
    /// @brief Worker 循环: DEALER socket 负责处理请求
    void workerLoop(const std::string& workers_addr) {
        zmq::socket_t worker(context_, zmq::socket_type::dealer);
        worker.set(zmq::sockopt::linger, 0);
        
        worker.connect(workers_addr);  // worker connect to frontend's bind
        
        LOG_DEBUG("[ZmqService::worker] Worker 线程已启动");
        
        while (running_.load()) {
            try {
                zmq::message_t client_id, empty, request;
                
                // 接收: [client_id] [] [request]
                if (!worker.recv(client_id, zmq::recv_flags::none)) continue;
                if (!worker.recv(empty, zmq::recv_flags::none)) continue;
                if (!worker.recv(request, zmq::recv_flags::none)) continue;
                
                // 解析请求
                std::string req_str(
                    static_cast<const char*>(request.data()),
                    request.size()
                );
                Request req;
                req.ParseFromString(req_str);
                
                // 查找 handler
                typename IService<Request, Response>::Handler handler;
                {
                    std::lock_guard<std::mutex> lock(handlers_mutex_);
                    auto it = handlers_.find(req.endpoint());
                    if (it != handlers_.end()) {
                        handler = it->second;
                    } else {
                        LOG_WARN("[ZmqService::worker] 未知端点: %s", req.endpoint().c_str());
                        // 发送错误响应
                        Response resp;
                        resp.set_success(false);
                        resp.set_data("unknown endpoint: " + req.endpoint());
                        std::string resp_str;
                        resp.SerializeToString(&resp_str);
                        
                        worker.send(client_id, zmq::send_flags::sndmore);
                        worker.send(zmq::message_t(), zmq::send_flags::sndmore);
                        worker.send(zmq::message_t(resp_str.data(), resp_str.size()), 
                                   zmq::send_flags::none);
                        continue;
                    }
                }
                
                LOG_DEBUG("[ZmqService::worker] 处理请求: %s", req.endpoint().c_str());
                
                // 处理请求 (在 worker 线程中执行, 不阻塞其他请求)
                Response resp = handler(req);
                
                // 序列化响应
                std::string resp_str;
                resp.SerializeToString(&resp_str);
                
                LOG_DEBUG("[ZmqService::worker] 发送响应: %s", req.endpoint().c_str());
                
                // 发送响应: [client_id] [] [response]
                zmq::message_t response(resp_str.data(), resp_str.size());
                worker.send(client_id, zmq::send_flags::sndmore);
                worker.send(zmq::message_t(), zmq::send_flags::sndmore);  // 空帧
                worker.send(response, zmq::send_flags::none);
                
            } catch (const std::exception& e) {
                LOG_ERROR("[ZmqService::worker] 处理错误: %s", e.what());
            }
        }
        
        LOG_DEBUG("[ZmqService::worker] Worker 线程已停止");
    }

    /// @brief 初始化客户端连接池 (仅预热连接，不共享 socket)
    void initializeClientPool() {
        uint16_t port = computeServicePort(name_, base_port_);
        std::string addr = "tcp://localhost:" + std::to_string(port);
        
        std::lock_guard<std::mutex> lock(client_pool_mutex_);
        
        LOG_INFO("[ZmqService] 客户端连接池预热: %d 个连接", client_pool_size_);
        
        // 预热连接：创建 socket 并 connect，然后立即销毁
        // 这样 TCP 连接会保持在 TIME_WAIT 状态，后续 connect 更快
        for (int i = 0; i < client_pool_size_; ++i) {
            auto socket = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::req);
            socket->set(zmq::sockopt::linger, 0);
            
            try {
                socket->connect(addr);
                // 发送一个空消息触发连接建立
                zmq::message_t empty_msg(0);
                socket->send(empty_msg, zmq::send_flags::none);
                
                // 立即销毁 socket，但 TCP 连接会被操作系统缓存
                socket.reset();
                
                LOG_DEBUG("[ZmqService] 连接池预热 [%d/%d]: %s", 
                         i + 1, client_pool_size_, addr.c_str());
            } catch (const zmq::error_t& e) {
                LOG_ERROR("[ZmqService] 连接池预热失败: %s", e.what());
            }
        }
        
        client_pool_initialized_ = true;
        LOG_INFO("[ZmqService] 客户端连接池预热完成");
    }
    
    /// @brief 从连接池获取 socket (每个调用独立 socket，但连接已预热)
    std::unique_ptr<zmq::socket_t> getClientSocket() {
        uint16_t port = computeServicePort(name_, base_port_);
        std::string addr = "tcp://localhost:" + std::to_string(port);
        
        auto socket = std::make_unique<zmq::socket_t>(context_, zmq::socket_type::req);
        socket->set(zmq::sockopt::linger, 0);
        socket->set(zmq::sockopt::sndtimeo, 5000);
        socket->set(zmq::sockopt::rcvtimeo, 5000);
        socket->connect(addr);
        
        return socket;
    }
    
    /// @brief 清理客户端连接池
    void clearClientPool() {
        std::lock_guard<std::mutex> lock(client_pool_mutex_);
        client_pool_initialized_ = false;
        LOG_INFO("[ZmqService] 客户端连接池已清理");
    }
    
    zmq::context_t& context_;
    std::string name_;
    uint16_t base_port_;
    int num_workers_;  // Worker 线程数量
    std::map<std::string, typename IService<Request, Response>::Handler> handlers_;
    std::mutex handlers_mutex_;
    std::atomic<bool> running_;
    
    std::thread frontend_thread_;       // 前端 ROUTER 线程
    std::vector<std::thread> workers_;  // Worker 线程池
    
    // 客户端连接池 (用于预热连接，不共享 socket)
    std::mutex client_pool_mutex_;
    int client_pool_size_ = 2;  // 预热连接数
    bool client_pool_initialized_ = false;
};
