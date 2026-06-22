#pragma once
#include "rpc/message_types.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>

/// @brief 端点描述：权限、限流、说明
struct EndpointDesc {
    std::string name;             // 端点名称，如 "set_exposure"
    std::string description;      // 人类可读说明
    bool requires_admin = false; // 是否需要管理员权限
    int rate_limit_per_sec = 0;  // 每秒最大请求数（0=不限）
};

/// @brief 端点处理函数：接收 ServiceRequest，返回 ServiceResponse
using EndpointHandler = std::function<ServiceResponse(const ServiceRequest&)>;

/// @brief 统一端点注册表：
/// - 集中管理所有服务端点
/// - 权限检查（requires_admin）
/// - 请求频率限制
class ServiceEndpointRegistry {
public:
    ServiceEndpointRegistry() = default;
    ~ServiceEndpointRegistry() = default;

    /// @brief 注册一个端点
    /// @param desc 端点描述（权限、限流、说明）
    /// @param handler 处理函数
    void registerEndpoint(const EndpointDesc& desc, EndpointHandler handler);

    /// @brief 处理请求：执行权限检查 -> 限流检查 -> 调用 handler
    /// @param endpoint_name 端点名
    /// @param req 请求
    /// @param is_admin 调用者是否有管理员权限
    /// @return 响应
    ServiceResponse handle(const std::string& endpoint_name,
                           const ServiceRequest& req,
                           bool is_admin = false);

    /// @brief 获取所有已注册端点的列表（debug 用）
    std::vector<EndpointDesc> listEndpoints() const;

    /// @brief 检查某个端点是否已注册
    bool hasEndpoint(const std::string& name) const;

    /// @brief 重置所有限流计数器（可由定时任务周期性调用）
    void resetRateLimitCounters();

private:
    /// @brief 内部的端点条目
    struct EndpointEntry {
        EndpointDesc desc;
        EndpointHandler handler;
        std::atomic<int> request_count{0};  // 当前周期请求数
        std::chrono::steady_clock::time_point last_reset;
        std::mutex counter_mutex;
    };

    std::map<std::string, std::shared_ptr<EndpointEntry>> endpoints_;
    mutable std::mutex endpoints_mutex_;

    /// @brief 检查是否超过限流阈值
    bool checkRateLimit(EndpointEntry& entry);

    /// @brief 构建 "权限不足" 响应
    static ServiceResponse permissionDenied(const std::string& endpoint);

    /// @brief 构建 "限流" 响应
    static ServiceResponse rateLimited(const std::string& endpoint);

    /// @brief 构建 "端点未找到" 响应
    static ServiceResponse notFound(const std::string& endpoint);
};
