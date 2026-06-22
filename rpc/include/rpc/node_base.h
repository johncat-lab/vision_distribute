#pragma once
#include "rpc/node_manifest.h"
#include "rpc/message_types.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>

class NodeEdgeManager;  // 前向声明
class NodeContainer;     // 前向声明
class ServiceRegistry;   // 前向声明
class ServiceEndpointRegistry; // 前向声明

/// @brief 统一节点基类：
/// - 将节点的数据流（pub-sub）和服务调用（RPC）两个通道解耦
/// - 声明式描述端口和服务端点
/// - 子类只需实现 describe() + 具体的处理函数
class NodeBase {
public:
    virtual ~NodeBase() = default;

    /// @brief 返回节点的声明式描述（端口、服务、版本等）
    virtual NodeManifest describe() const = 0;

    /// @brief 初始化数据流通道（订阅输入、声明输出）
    /// @param edges 端口映射管理器，用来获取 pub/sub
    /// @param config_file 可选的 config 文件路径（从命令行传入）
    virtual void initDataflow(NodeEdgeManager& edges,
                               const std::string& config_file) {}

    /// @brief 初始化服务通道（注册提供的服务端点）
    /// @param services 端点注册表，用来注册 RPC 处理函数
    virtual void initServices(ServiceEndpointRegistry& services) = 0;

    /// @brief 初始化服务通道（注册端点 + ROS2 原生类型映射）
    /// 默认实现委托给 initServices(ServiceEndpointRegistry&)。
    /// 需要 ROS2 支持的节点应覆盖此方法，通过 container.registerRos2NativeEndpoint()
    /// 注册 .srv 类型映射。
    /// @param services 端点注册表
    /// @param container 节点容器，可调用 registerRos2NativeEndpoint<SrvType>()
    virtual void initServices(ServiceEndpointRegistry& services, NodeContainer& container) {
        (void)container;
        initServices(services);
    }

    /// @brief 主循环开始前调用（可选：打开硬件、加载模型等）
    /// @return true=启动成功
    virtual bool start() { return true; }

    /// @brief 主循环结束后调用（可选：关闭硬件、释放资源）
    virtual void stop() {}

    /// @brief 节点主循环（每 tick 一次）
    /// @param running 可通过此原子标志优雅退出
    virtual void tick(std::atomic<bool>& running) {
        (void)running;
    }
};

/// @brief 便捷宏：子类中用一行定义节点描述
#define NODE_MANIFEST(name_, binary_, version_, config_) \
    NodeManifest buildManifest() const override { \
        NodeManifest m; \
        m.name = name_; \
        m.binary = binary_; \
        m.version = version_; \
        m.config_file = config_; \
        return m; \
    }
