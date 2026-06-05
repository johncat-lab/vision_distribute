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

#ifdef HAS_ROS2
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

// ========== ROS2 全局上下文 (定义在 ros2_backend.cpp) ==========
namespace ros2_global {
    void init(const std::string& node_name);
    void shutdown();
    rclcpp::Node* getNode();
}

// ========== ROS2 Publisher ==========
// 使用 std_msgs::msg::String 承载序列化后的二进制消息
template<typename T>
class Ros2Publisher : public IPublisher<T> {
public:
    Ros2Publisher(const std::string& topic)
        : topic_(topic) {
        auto* node = ros2_global::getNode();
        if (!node) {
            throw std::runtime_error("ROS2 node not initialized");
        }
        // 使用可靠的 QoS 策略保证帧数据不丢失
        auto qos = rclcpp::QoS(rclcpp::KeepLast(10)).reliable();
        pub_ = node->create_publisher<std_msgs::msg::String>(topic, qos);
    }

    bool publish(const T& msg) override {
        try {
            auto ros_msg = std::make_unique<std_msgs::msg::String>();
            ros_msg->data = msg.serialize();
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
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_;
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
        sub_ = node->create_subscription<std_msgs::msg::String>(
            topic, qos,
            [this](std::unique_ptr<std_msgs::msg::String> msg) {
                std::lock_guard<std::mutex> lock(cb_mutex_);
                if (callback_) {
                    try {
                        T deserialized = T::deserialize(msg->data);
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
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_;
    typename ISubscriber<T>::Callback callback_;
    std::mutex cb_mutex_;
};

// ========== ROS2 Service (基于 topic 的请求/响应模式) ==========
//
// 由于 ROS2 service 需要预定义的 .srv 文件，与模板架构不兼容，
// 这里使用一对 pub/sub topic 来模拟 service 调用:
//   - 请求 topic: {service_name}/_req
//   - 响应 topic: {service_name}/_res
//
// 每条消息通过 4 字节 correlation_id 头来匹配请求与响应。
// 请求格式: [4B corr_id][4B payload_len][payload_len B: Request::serialize()]
// 响应格式: [4B corr_id][4B payload_len][payload_len B: Response::serialize()]

template<typename Request, typename Response>
class Ros2Service : public IService<Request, Response> {
    using Handler = typename IService<Request, Response>::Handler;

    // 内部请求包装
    struct SvcRequest {
        uint32_t corr_id = 0;
        std::string payload;

        std::string serialize() const {
            uint32_t len = static_cast<uint32_t>(payload.size());
            std::string buf(4 + 4 + len, '\0');
            char* p = &buf[0];
            std::memcpy(p, &corr_id, 4);  p += 4;
            std::memcpy(p, &len, 4);      p += 4;
            if (len > 0) std::memcpy(p, payload.data(), len);
            return buf;
        }
        static SvcRequest deserialize(const std::string& data) {
            SvcRequest r;
            if (data.size() < 8) return r;
            const char* p = data.data();
            std::memcpy(&r.corr_id, p, 4);  p += 4;
            uint32_t len = 0;
            std::memcpy(&len, p, 4);        p += 4;
            if (len > 0 && data.size() >= 8 + len)
                r.payload.assign(p, len);
            return r;
        }
    };

    // 内部响应包装
    struct SvcResponse {
        uint32_t corr_id = 0;
        std::string payload;

        std::string serialize() const {
            uint32_t len = static_cast<uint32_t>(payload.size());
            std::string buf(4 + 4 + len, '\0');
            char* p = &buf[0];
            std::memcpy(p, &corr_id, 4);  p += 4;
            std::memcpy(p, &len, 4);      p += 4;
            if (len > 0) std::memcpy(p, payload.data(), len);
            return buf;
        }
        static SvcResponse deserialize(const std::string& data) {
            SvcResponse r;
            if (data.size() < 8) return r;
            const char* p = data.data();
            std::memcpy(&r.corr_id, p, 4);  p += 4;
            uint32_t len = 0;
            std::memcpy(&len, p, 4);        p += 4;
            if (len > 0 && data.size() >= 8 + len)
                r.payload.assign(p, len);
            return r;
        }
    };

public:
    explicit Ros2Service(const std::string& name)
        : name_(name) {
        auto* node = ros2_global::getNode();
        if (!node) {
            throw std::runtime_error("ROS2 node not initialized");
        }
        node_ = node;

        // 提前创建响应 topic 的 subscriber（所有 call() 共享）
        auto qos = rclcpp::QoS(rclcpp::KeepLast(10)).reliable();
        res_sub_ = node_->create_subscription<std_msgs::msg::String>(
            name_ + "/_res", qos,
            [this](std::unique_ptr<std_msgs::msg::String> msg) {
                SvcResponse svc_resp = SvcResponse::deserialize(msg->data);
                std::lock_guard<std::mutex> lock(pending_mutex_);
                auto it = pending_.find(svc_resp.corr_id);
                if (it != pending_.end()) {
                    try {
                        Response resp = Response::deserialize(svc_resp.payload);
                        it->second.set_value(std::move(resp));
                    } catch (...) {
                        // 反序列化失败，promise 将因异常而 broken
                        try { it->second.set_exception(std::current_exception()); } catch (...) {}
                    }
                    pending_.erase(it);
                }
            });
    }

    ~Ros2Service() {
        stop();
    }

    bool serve(const std::string& endpoint,
               Handler handler) override {
        (void)endpoint;  // endpoint 在 ServiceRequest 中携带，此处不需要
        stop();

        handler_ = std::move(handler);

        // 创建请求 topic subscriber
        auto qos = rclcpp::QoS(rclcpp::KeepLast(10)).reliable();
        req_sub_ = node_->create_subscription<std_msgs::msg::String>(
            name_ + "/_req", qos,
            [this](std::unique_ptr<std_msgs::msg::String> msg) {
                if (!handler_) return;

                SvcRequest svc_req = SvcRequest::deserialize(msg->data);
                Request req;
                try {
                    req = Request::deserialize(svc_req.payload);
                } catch (const std::exception& e) {
                    std::cerr << "ROS2 service '" << name_
                              << "' deserialize error: " << e.what() << std::endl;
                    return;
                }

                // 调用业务 handler
                Response resp;
                try {
                    resp = handler_(req);
                } catch (const std::exception& e) {
                    std::cerr << "ROS2 service '" << name_
                              << "' handler error: " << e.what() << std::endl;
                    resp = Response();  // 返回默认空响应
                }

                // 发布响应
                if (res_pub_) {
                    SvcResponse svc_resp;
                    svc_resp.corr_id = svc_req.corr_id;
                    svc_resp.payload = resp.serialize();
                    auto ros_msg = std::make_unique<std_msgs::msg::String>();
                    ros_msg->data = svc_resp.serialize();
                    res_pub_->publish(std::move(ros_msg));
                }
            });

        // 创建响应 topic publisher
        res_pub_ = node_->create_publisher<std_msgs::msg::String>(
            name_ + "/_res", qos);

        return true;
    }

    Response call(const std::string& endpoint, const Request& req) override {
        (void)endpoint;

        // 生成唯一 correlation ID
        static std::atomic<uint32_t> next_id{1};
        uint32_t corr_id = next_id.fetch_add(1, std::memory_order_relaxed);
        if (corr_id == 0) corr_id = next_id.fetch_add(1, std::memory_order_relaxed);

        // 注册 pending promise
        std::promise<Response> promise;
        std::future<Response> future = promise.get_future();
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_[corr_id] = std::move(promise);
        }

        // 确保请求 publisher 存在
        if (!req_pub_) {
            auto qos = rclcpp::QoS(rclcpp::KeepLast(10)).reliable();
            req_pub_ = node_->create_publisher<std_msgs::msg::String>(
                name_ + "/_req", qos);
        }

        // 发布请求
        SvcRequest svc_req;
        svc_req.corr_id = corr_id;
        svc_req.payload = req.serialize();
        auto ros_msg = std::make_unique<std_msgs::msg::String>();
        ros_msg->data = svc_req.serialize();
        req_pub_->publish(std::move(ros_msg));

        // 等待响应（带超时）
        auto status = future.wait_for(std::chrono::seconds(5));
        if (status == std::future_status::timeout) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_.erase(corr_id);
            throw std::runtime_error("ROS2 service call timeout: " + name_ + "/" + endpoint);
        }

        try {
            return future.get();
        } catch (const std::exception&) {
            throw std::runtime_error("ROS2 service call failed: " + name_ + "/" + endpoint);
        }
    }

private:
    void stop() {
        req_sub_.reset();
        res_pub_.reset();
        handler_ = nullptr;
    }

    std::string name_;
    rclcpp::Node* node_ = nullptr;

    // 服务端
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr req_sub_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr res_pub_;
    Handler handler_;

    // 客户端（请求/响应匹配）
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr req_pub_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr res_sub_;
    std::mutex pending_mutex_;
    std::map<uint32_t, std::promise<Response>> pending_;
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
