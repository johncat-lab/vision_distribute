#pragma once
#include "rpc/node_factory.h"
#include "rpc/message_types.h"
#include "rpc/service.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <chrono>

/// @brief Service 端点信息
struct ServiceEndpoint {
    std::string instance_name;
    std::string service_name;
    std::string role;
};

/// @brief 传输无关的 Service 角色发现注册表
/// 复用现有 IService 基础设施，不引入新通信通道。
class ServiceRegistry {
public:
    /// @param factory 已有的 NodeFactory 实例（自动适配 ZMQ/ROS2/Zenoh）
    explicit ServiceRegistry(NodeFactory& factory);

    /// @brief 注册自己的 service role
    /// @param role 服务角色（如 "camera", "detector"）
    /// @param instance_name 实例名（如 "cam_left"）
    /// @param service_name service 名（如 "camera"）
    void registerRole(const std::string& role,
                      const std::string& instance_name,
                      const std::string& service_name);

    /// @brief 按角色查找服务提供者
    std::vector<ServiceEndpoint> discover(const std::string& role) const;

    /// @brief 等待某个角色的服务可用
    /// @param role 服务角色
    /// @param timeout_ms 超时时间（毫秒）
    /// @return true=在超时前找到
    bool waitForRole(const std::string& role, int timeout_ms) const;

    /// @brief 创建指向某 role 的 service client
    /// @param role 服务角色
    /// @param index 如果有多个 provider，选第几个（默认 0）
    std::shared_ptr<IService<ServiceRequest, ServiceResponse>>
    createClient(const std::string& role, int index = 0);

    /// @brief 注销某实例的所有注册
    void unregister(const std::string& instance_name);

private:
    NodeFactory& factory_;
    mutable std::mutex mutex_;
    // role → [(instance_name, service_name)]
    std::map<std::string, std::vector<ServiceEndpoint>> registry_;
};
