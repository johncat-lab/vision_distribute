#pragma once
#include "rpc/publisher.h"
#include "rpc/subscriber.h"
#include "rpc/service.h"
#include <string>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <map>
#include <future>
#include <atomic>
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

#ifdef HAS_ROS2
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/byte_multi_array.hpp>
#include <rcl_interfaces/srv/get_parameter_types.hpp>

// ========== ROS2 全局上下文 (定义在 ros2_backend.cpp) ==========
namespace ros2_global {
    void init(const std::string& node_name);
    void shutdown();
    rclcpp::Node* getNode();
}

// ========== 内部工具：base64 编解码 ==========
// 用于将任意二进制数据编码为 DDS string 字段安全的 ASCII 字符串
namespace {

static const char kB64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string b64_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(kB64Chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(kB64Chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

inline std::string b64_decode(const std::string& in) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(unsigned char)kB64Chars[i]] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back((char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

} // anonymous namespace

// ========== ROS2 Publisher ==========
// 使用 std_msgs::msg::ByteMultiArray 承载序列化后的二进制消息
// (String 内部使用 strlen() 会被 \0 截断，ByteMultiArray 用 vector<uint8_t> 正确传输)
template<typename T>
class Ros2Publisher : public IPublisher<T> {
public:
    Ros2Publisher(const std::string& topic)
        : topic_(topic) {
        auto* node = ros2_global::getNode();
        if (!node) {
            throw std::runtime_error("ROS2 node not initialized");
        }
        auto qos = rclcpp::QoS(rclcpp::KeepLast(10)).reliable();
        pub_ = node->create_publisher<std_msgs::msg::ByteMultiArray>(topic, qos);
    }

    bool publish(const T& msg) override {
        try {
            auto ros_msg = std::make_unique<std_msgs::msg::ByteMultiArray>();
            const std::string& raw = msg.serialize();
            ros_msg->data.assign(raw.begin(), raw.end());
            pub_->publish(std::move(ros_msg));
            return true;
        } catch (const std::exception& e) {
            std::cerr << "ROS2 publish error on '" << topic_ << "': " << e.what() << std::endl;
            return false;
        }
    }

    std::string getTopic() const override { return topic_; }

private:
    std::string topic_;
    rclcpp::Publisher<std_msgs::msg::ByteMultiArray>::SharedPtr pub_;
};

// ========== ROS2 Subscriber ==========
template<typename T>
class Ros2Subscriber : public ISubscriber<T> {
public:
    Ros2Subscriber(const std::string& topic)
        : topic_(topic) {
        auto* node = ros2_global::getNode();
        if (!node) {
            throw std::runtime_error("ROS2 node not initialized");
        }
        auto qos = rclcpp::QoS(rclcpp::KeepLast(10)).reliable();
        sub_ = node->create_subscription<std_msgs::msg::ByteMultiArray>(
            topic, qos,
            [this](std::unique_ptr<std_msgs::msg::ByteMultiArray> msg) {
                std::lock_guard<std::mutex> lock(cb_mutex_);
                if (callback_) {
                    try {
                        std::string raw(msg->data.begin(), msg->data.end());
                        T deserialized = T::deserialize(raw);
                        callback_(deserialized);
                    } catch (const std::exception& e) {
                        std::cerr << "ROS2 subscriber deserialize error on '"
                                  << topic_ << "': " << e.what() << std::endl;
                    }
                }
            });
    }

    bool subscribe(typename ISubscriber<T>::Callback cb) override {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        callback_ = std::move(cb);
        return true;
    }

    std::string getTopic() const override { return topic_; }

private:
    std::string topic_;
    rclcpp::Subscription<std_msgs::msg::ByteMultiArray>::SharedPtr sub_;
    typename ISubscriber<T>::Callback callback_;
    std::mutex cb_mutex_;
};

// ========== ROS2 Service (基于原生 rclcpp::Service/Client) ==========
//
// 使用 rcl_interfaces/srv/GetParameterTypes 作为通用二进制载体：
//   request.names[0]  = base64(ServiceRequest::serialize())  — 避免 DDS string \0 截断
//   response.types    = ServiceResponse::serialize() 的原始字节（uint8[]，无需编码）
//
// 一个 Ros2Service 实例对应一个 native service（名称 = name_）。
// 多个 endpoint 通过 ServiceRequest.endpoint 字段在 handler map 内部路由。
// native service 内置请求/响应匹配，彻底消除 DDS 发现时序问题。

template<typename Request, typename Response>
class Ros2Service : public IService<Request, Response> {
    using Handler = typename IService<Request, Response>::Handler;
    using SrvType = rcl_interfaces::srv::GetParameterTypes;

public:
    explicit Ros2Service(const std::string& name)
        : name_(name) {
        auto* node = ros2_global::getNode();
        if (!node) {
            throw std::runtime_error("ROS2 node not initialized");
        }
        node_ = node;
    }

    bool serve(const std::string& endpoint, Handler handler) override {
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            handlers_[endpoint] = std::move(handler);
        }

        // 首次调用时创建 native service（只创建一次，后续 serve 仅注册 handler）
        if (!ros2_service_) {
            ros2_service_ = node_->create_service<SrvType>(
                name_,
                [this](const std::shared_ptr<SrvType::Request> req,
                       std::shared_ptr<SrvType::Response> resp) {
                    // 解码请求：base64 -> 原始字节 -> Request
                    if (req->names.empty()) {
                        std::cerr << "[Ros2Service::serve " << name_
                                  << "] empty request" << std::endl;
                        return;
                    }
                    std::string raw = b64_decode(req->names[0]);
                    Request sreq;
                    try {
                        sreq = Request::deserialize(raw);
                    } catch (const std::exception& e) {
                        std::cerr << "[Ros2Service::serve " << name_
                                  << "] deserialize error: " << e.what() << std::endl;
                        return;
                    }

                    std::cerr << "[Ros2Service::serve " << name_
                              << "] endpoint='" << sreq.endpoint << "'" << std::endl;

                    // 查找 handler
                    Handler h;
                    {
                        std::lock_guard<std::mutex> lock(handlers_mutex_);
                        auto it = handlers_.find(sreq.endpoint);
                        if (it == handlers_.end()) {
                            std::cerr << "[Ros2Service::serve " << name_
                                      << "] unknown endpoint: " << sreq.endpoint << std::endl;
                            return;
                        }
                        h = it->second;
                    }

                    // 调用业务 handler
                    Response sresp;
                    try {
                        sresp = h(sreq);
                    } catch (const std::exception& e) {
                        std::cerr << "[Ros2Service::serve " << name_
                                  << "] handler error: " << e.what() << std::endl;
                        sresp = Response();
                    }

                    // 编码响应：原始字节 -> uint8[]（直接存储，无需 base64）
                    std::string bytes = sresp.serialize();
                    resp->types.assign(bytes.begin(), bytes.end());

                    std::cerr << "[Ros2Service::serve " << name_
                              << "] response size=" << bytes.size() << "B" << std::endl;
                });

            std::cerr << "[Ros2Service] " << name_
                      << " native service created, endpoint=" << endpoint << std::endl;
        } else {
            std::cerr << "[Ros2Service] " << name_
                      << " handler registered: " << endpoint << std::endl;
        }

        return true;
    }

    Response call(const std::string& endpoint, const Request& req) override {
        (void)endpoint;

        auto t0 = std::chrono::steady_clock::now();
        auto ms = [&t0]() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
        };

        // 惰性创建 client
        std::call_once(client_init_flag_, [this]() {
            ros2_client_ = node_->create_client<SrvType>(name_);
        });

        // 不调用 wait_for_service()：
        // wait_for_service() 内部会启动临时 executor 轮询，
        // 与已运行的 MultiThreadedExecutor 竞争节点所有权，
        // 导致 response callback 被临时 executor 消费后丢失，promise 永远不被填充。
        // 直接发请求，promise 超时自然覆盖 service 不可用的情况。
        std::cerr << "[Ros2Service::call] " << name_ << "/" << endpoint
                  << " +0ms sending..." << std::endl;

        auto ros_req = std::make_shared<SrvType::Request>();
        ros_req->names.push_back(b64_encode(req.serialize()));

        auto promise = std::make_shared<std::promise<typename SrvType::Response::SharedPtr>>();
        auto std_future = promise->get_future();

        ros2_client_->async_send_request(
            ros_req,
            [promise, name = name_, endpoint, ms](rclcpp::Client<SrvType>::SharedFuture f) {
                std::cerr << "[Ros2Service::callback] " << name << "/" << endpoint
                          << " +" << ms() << "ms callback fired!" << std::endl;
                promise->set_value(f.get());
            });

        if (std_future.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
            std::cerr << "[Ros2Service::call] " << name_ << "/" << endpoint
                      << " +" << ms() << "ms TIMEOUT, callback never fired" << std::endl;
            throw std::runtime_error(
                "ROS2 service call timeout: " + name_ + "/" + endpoint);
        }

        auto ros_resp = std_future.get();
        std::string bytes(ros_resp->types.begin(), ros_resp->types.end());

        std::cerr << "[Ros2Service::call] " << name_ << "/" << endpoint
                  << " +" << ms() << "ms response " << bytes.size() << "B" << std::endl;

        return Response::deserialize(bytes);
    }

private:
    std::string name_;
    rclcpp::Node* node_ = nullptr;

    // 服务端
    rclcpp::Service<SrvType>::SharedPtr ros2_service_;
    std::map<std::string, Handler> handlers_;
    std::mutex handlers_mutex_;

    // 客户端
    rclcpp::Client<SrvType>::SharedPtr ros2_client_;
    std::once_flag client_init_flag_;
};

#else  // !HAS_ROS2 — 存根实现

template<typename T>
class Ros2Publisher : public IPublisher<T> {
public:
    Ros2Publisher(const std::string& topic) : topic_(topic) {}
    bool publish(const T&) override {
        throw std::runtime_error("ROS2 backend not available (compile with -DHAS_ROS2)");
    }
    std::string getTopic() const override { return topic_; }
private:
    std::string topic_;
};

template<typename T>
class Ros2Subscriber : public ISubscriber<T> {
public:
    Ros2Subscriber(const std::string& topic) : topic_(topic) {}
    bool subscribe(typename ISubscriber<T>::Callback) override {
        throw std::runtime_error("ROS2 backend not available (compile with -DHAS_ROS2)");
    }
    std::string getTopic() const override { return topic_; }
private:
    std::string topic_;
};

template<typename Request, typename Response>
class Ros2Service : public IService<Request, Response> {
public:
    Ros2Service(const std::string& name) : name_(name) {}
    bool serve(const std::string&,
               typename IService<Request, Response>::Handler) override {
        throw std::runtime_error("ROS2 backend not available (compile with -DHAS_ROS2)");
    }
    Response call(const std::string&, const Request&) override {
        throw std::runtime_error("ROS2 backend not available (compile with -DHAS_ROS2)");
    }
private:
    std::string name_;
};

#endif  // HAS_ROS2
