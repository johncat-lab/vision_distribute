#pragma once
#include "rpc/publisher.h"
#include "rpc/subscriber.h"
#include "rpc/service.h"
#include "rpc/message_types.h"
#include "logger/logger.h"
#include <string>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <map>
#include <future>
#include <atomic>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <set>

#ifdef HAS_ROS2
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/byte_multi_array.hpp>
// 注意: 不再使用 GetParameterTypes 作为通用载体
// 各 endpoint 直接使用原生 .srv 类型，由 registerNativeEndpoint<SrvType> 注册

// ========== ROS2 全局上下文 (定义在 ros2_backend.cpp) ==========
namespace ros2_global {
    void init(const std::string& node_name, int executor_threads = 4);
    void start_executor();
    void shutdown();
    rclcpp::Node* getNode();
}

// base64 编解码已移除 — 原生 service 通道使用类型化字段传输，不再需要 string↔binary 转换

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
        // 使用 BestEffort + KeepLast(1) 避免大帧数据拥塞 DDS 传输层
        auto qos = rclcpp::QoS(rclcpp::KeepLast(1)).best_effort();
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
            LOG_ERROR("ROS2 publish error on '%s': %s", topic_.c_str(), e.what());
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
        // 使用 BestEffort + KeepLast(1) 避免大帧数据拥塞 DDS 传输层
        auto qos = rclcpp::QoS(rclcpp::KeepLast(1)).best_effort();
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
                        LOG_ERROR("ROS2 subscriber deserialize error on '%s': %s", topic_.c_str(), e.what());
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

// ========== ROS2 Service (纯原生 service 通道) ==========
//
// 每个 endpoint 对应一个原生 ROS2 service（如 /camera/set_exposure），
// 不再使用 GetParameterTypes 通用载体，消除 base64 编解码和双通道冗余。
//
// registerNativeEndpoint<SrvType> 注册 4 个转换 lambda:
//   toSrvReq:    原生请求 → ServiceRequest (服务端)
//   fromSrvResp: ServiceResponse → 原生响应 (服务端)
//   toNativeReq: ServiceRequest → 原生请求 (客户端)
//   fromNativeResp: 原生响应 → ServiceResponse (客户端)
//
// serve() 为该 endpoint 创建原生 service。
// call() 通过原生 client 调用，自动做 ServiceRequest ↔ 原生请求转换。
// preconnect() 预创建所有已注册 endpoint 的原生 client。

template<typename Request, typename Response>
class Ros2Service : public IService<Request, Response> {
    using Handler = typename IService<Request, Response>::Handler;

public:
    explicit Ros2Service(const std::string& name, int timeout_ms = 5000,
                         int wait_ms = 3000, int max_retries = 3)
        : name_(name), timeout_ms_(timeout_ms), wait_ms_(wait_ms), max_retries_(max_retries)
    {
        auto* node = ros2_global::getNode();
        if (!node) {
            throw std::runtime_error("ROS2 node not initialized");
        }
        node_ = node;
    }

    // ========== 预连接：提前创建所有已注册 endpoint 的原生 client ==========
    void preconnect() override {
        std::lock_guard<std::mutex> lock(native_mutex_);
        for (auto& [endpoint, config] : native_srv_configs_) {
            config.preconnect_client(node_);
        }
    }

    // ========== 注册原生 ROS2 service 类型映射 (双向转换) ==========
    // 在 serve() 前调用，声明端点对应的 .srv 类型及双向请求/响应转换。
    // serve() 自动为已注册的端点创建原生 ROS2 service。
    // call() 自动为已注册的端点创建原生 client 并做类型转换。
    //
    // @param endpoint      端点名（如 "set_exposure")
    // @param toSrvReq      原生请求 → ServiceRequest (服务端接收时使用)
    // @param fromSrvResp   ServiceResponse → 原生响应 (服务端返回时使用)
    // @param toNativeReq   ServiceRequest → 原生请求 (客户端发送时使用)
    // @param fromNativeResp 原生响应 → ServiceResponse (客户端接收时使用)
    template<typename SrvType>
    void registerNativeEndpoint(
        const std::string& endpoint,
        std::function<ServiceRequest(const std::shared_ptr<typename SrvType::Request>&)> toSrvReq,
        std::function<void(const ServiceResponse&, std::shared_ptr<typename SrvType::Response>)> fromSrvResp,
        std::function<std::shared_ptr<typename SrvType::Request>(const ServiceRequest&)> toNativeReq,
        std::function<ServiceResponse(const std::shared_ptr<typename SrvType::Response>&)> fromNativeResp)
    {
        std::string global_name = (name_.empty() || name_[0] == '/') ? name_ : "/" + name_;
        std::string full_name = global_name + "/" + endpoint;

        // 客户端持有者: shared_ptr<rclcpp::Client<SrvType>::SharedPtr>
        // 类型擦除: lambda 内部捕获此 holder，外部仅通过 std::function 调用
        auto client_holder = std::make_shared<typename rclcpp::Client<SrvType>::SharedPtr>();
        auto client_mtx = std::make_shared<std::mutex>();

        // 显式构造 std::function 以避免 GCC 对模板内局部类型聚合初始化的限制
        std::function<void(rclcpp::Node*, Handler)> create_srv_fn =
            [full_name, toSrvReq = std::move(toSrvReq), fromSrvResp = std::move(fromSrvResp),
             services = services_holder_]
            (rclcpp::Node* node, Handler handler) {
                auto srv = node->create_service<SrvType>(
                    full_name,
                    [handler = std::move(handler), toSrvReq, fromSrvResp, full_name]
                    (const std::shared_ptr<typename SrvType::Request> req,
                     std::shared_ptr<typename SrvType::Response> resp) {
                        LOG_INFO("[Ros2Service::native] %s 收到请求", full_name.c_str());
                        try {
                            auto sreq = toSrvReq(req);
                            auto sresp = handler(sreq);
                            fromSrvResp(sresp, resp);
                        } catch (const std::exception& e) {
                            LOG_ERROR("[Ros2Service::native] %s 异常: %s",
                                      full_name.c_str(), e.what());
                        }
                    });
                services->push_back(srv);
                LOG_INFO("[Ros2Service] 原生 service 已创建: %s", full_name.c_str());
            };

        std::function<void(rclcpp::Node*)> preconnect_client_fn =
            [full_name, client_holder, client_mtx]
            (rclcpp::Node* node) {
                std::lock_guard<std::mutex> lock(*client_mtx);
                if (!*client_holder) {
                    *client_holder = node->create_client<SrvType>(full_name);
                    LOG_INFO("[Ros2Service] 原生 client 预连接: %s", full_name.c_str());
                }
            };

        // 捕获成员变量值而非 this 指针，避免模板上下文中 GCC 对 local type 的限制
        int timeout_ms = timeout_ms_;
        int max_retries = max_retries_;

        std::function<ServiceResponse(rclcpp::Node*, const ServiceRequest&)> call_native_fn =
            [full_name, toNativeReq = std::move(toNativeReq), fromNativeResp = std::move(fromNativeResp),
             client_holder, client_mtx, timeout_ms, max_retries]
            (rclcpp::Node* node, const ServiceRequest& req) -> ServiceResponse {
                // 创建 client if needed
                {
                    std::lock_guard<std::mutex> lock(*client_mtx);
                    if (!*client_holder) {
                        *client_holder = node->create_client<SrvType>(full_name);
                        LOG_INFO("[Ros2Service] 原生 client 已创建: %s", full_name.c_str());
                    }
                }
                auto client = *client_holder;

                // 注意: 不调用 wait_for_service()！
                // wait_for_service() 内部会启动临时 executor 轮询，
                // 与已运行的 MultiThreadedExecutor 竞争节点所有权，
                // 导致回调被临时 executor 消费后丢失。
                // 直接发请求，用 future 超时覆盖 service 不可用的情况。

                for (int retry = 0; retry < max_retries; ++retry) {
                    LOG_INFO("[Ros2Service::call] %s 发送请求 (尝试 %d/%d)",
                             full_name.c_str(), retry + 1, max_retries);

                    auto native_req = toNativeReq(req);
                    auto future = client->async_send_request(native_req);

                    auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
                    if (status == std::future_status::timeout) {
                        LOG_WARN("[Ros2Service::call] %s 超时 (重试 %d/%d)",
                                 full_name.c_str(), retry + 1, max_retries);
                        if (retry < max_retries - 1) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            continue;
                        }
                        throw std::runtime_error("ROS2 service call timeout: " + full_name);
                    }

                    auto native_resp = future.get();
                    return fromNativeResp(native_resp);
                }

                throw std::runtime_error("ROS2 service call failed: " + full_name);
            };

        std::lock_guard<std::mutex> lock(native_mutex_);
        native_srv_configs_[endpoint] = NativeEndpointConfig{
            full_name,
            std::move(create_srv_fn),
            std::move(preconnect_client_fn),
            std::move(call_native_fn)
        }; // NativeEndpointConfig end

        LOG_INFO("[Ros2Service] 类型映射已注册: %s → %s",
                 endpoint.c_str(), full_name.c_str());
    }

    bool serve(const std::string& endpoint, Handler handler) override {
        std::lock_guard<std::mutex> lock(native_mutex_);
        auto it = native_srv_configs_.find(endpoint);
        if (it == native_srv_configs_.end()) {
            LOG_ERROR("[Ros2Service::serve] 无类型映射的端点: %s/%s"
                      " — 必须先调用 registerNativeEndpoint",
                      name_.c_str(), endpoint.c_str());
            throw std::runtime_error(
                "No native endpoint registered for: " + name_ + "/" + endpoint);
        }

        if (native_services_created_.find(endpoint) == native_services_created_.end()) {
            it->second.create_srv(node_, handler);
            native_services_created_.insert(endpoint);
        } else {
            LOG_WARN("[Ros2Service::serve] %s/%s service 已存在，跳过重复创建",
                     name_.c_str(), endpoint.c_str());
        }
        return true;
    }

    Response call(const std::string& endpoint, const Request& req) override {
        std::lock_guard<std::mutex> lock(native_mutex_);
        auto it = native_srv_configs_.find(endpoint);
        if (it == native_srv_configs_.end()) {
            throw std::runtime_error(
                "No native endpoint registered for: " + name_ + "/" + endpoint);
        }
        // call_native 内部管理 client 创建/复用、等待可用、发送请求、接收响应
        return it->second.call_native(node_, req);
    }

private:
    std::string name_;
    rclcpp::Node* node_ = nullptr;

    // 超时/重试配置 (来自 NodeConfig)
    int timeout_ms_ = 5000;
    int wait_ms_ = 3000;
    int max_retries_ = 3;

    // === 类型擦除的原生 endpoint 配置 ===
    // 每个 endpoint 配置包含: 服务端 create_srv + 客户端 preconnect_client/call_native
    // 实际的 SrvType 在 registerNativeEndpoint<SrvType> 模板实例化时确定,
    // 通过 lambda 捕获实现类型擦除, 外部仅需调用 std::function
    struct NativeEndpointConfig {
        std::string full_name;
        std::function<void(rclcpp::Node*, Handler)> create_srv;
        std::function<void(rclcpp::Node*)> preconnect_client;
        std::function<ServiceResponse(rclcpp::Node*, const ServiceRequest&)> call_native;
    };
    std::map<std::string, NativeEndpointConfig> native_srv_configs_;
    std::set<std::string> native_services_created_;
    std::mutex native_mutex_;

    // 持有所有 create_service 返回的 shared_ptr，防止 service 对象被提前销毁
    std::shared_ptr<std::vector<rclcpp::ServiceBase::SharedPtr>> services_holder_ =
        std::make_shared<std::vector<rclcpp::ServiceBase::SharedPtr>>();
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
    Ros2Service(const std::string& name, int = 5000, int = 3000, int = 3) : name_(name) {}
    void preconnect() override {}
    bool serve(const std::string&,
               typename IService<Request, Response>::Handler) override {
        throw std::runtime_error("ROS2 backend not available (rebuild with rclcpp)");
    }
    Response call(const std::string&, const Request&) override {
        throw std::runtime_error("ROS2 backend not available (rebuild with rclcpp)");
    }
private:
    std::string name_;
};

#endif  // HAS_ROS2
