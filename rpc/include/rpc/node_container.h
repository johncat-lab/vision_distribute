#pragma once
#include "rpc/node_base.h"
#include "rpc/node_factory.h"
#include "rpc/edge_manager.h"
#include "rpc/service_endpoint_registry.h"
#include "rpc/message_types.h"
#include <memory>
#include <string>
#include <atomic>
#include <vector>

class ServiceRegistry;  // dag 模块，前向声明

/// @brief 节点容器：管理一个 NodeBase 的完整生命周期。
/// 职责：
/// - 解析命令行参数
/// - 创建传输层（NodeFactory）
/// - 调用节点的 initDataflow / initServices / start
/// - 将 ServiceEndpointRegistry 中注册的端点桥接到 IService 网络传输层
/// - 自动创建 ServiceRegistry 并注册角色（跨节点角色发现）
/// - 运行主循环，调用节点的 tick()
/// - 处理信号，优雅退出
class NodeContainer {
public:
    explicit NodeContainer(std::unique_ptr<NodeBase> node);
    ~NodeContainer();

    /// @brief 完整的节点运行入口，相当于 main()
    /// @return 进程退出码（0=正常，非0=错误）
    int run(int argc, char* argv[]);

    /// @brief 停止节点（由信号处理器或其他线程调用）
    void stop();

    /// @brief 手动触发一次 tick（主要用于测试）
    void tickOnce();

    /// @brief 获取内部 NodeFactory
    NodeFactory& factory() { return *factory_; }

    /// @brief 获取实例名（参数解析后可用）
    const std::string& instanceName() const { return instance_name_; }

    /// @brief 获取 ServiceRegistry（run() 后可用）
    ServiceRegistry* serviceRegistry() { return service_registry_.get(); }

private:
    /// @brief 将 ServiceEndpointRegistry 中的端点桥接到 IService 网络传输
    void bridgeServices();

    std::unique_ptr<NodeBase> node_;
    std::unique_ptr<NodeFactory> factory_;
    std::unique_ptr<NodeEdgeManager> edges_;
    std::unique_ptr<ServiceEndpointRegistry> services_;

    // IService 实例必须持有，否则服务线程会被销毁
    std::vector<std::shared_ptr<IService<ServiceRequest, ServiceResponse>>> service_instances_;

    // dag 模块的角色发现注册表（内部创建，复用 factory_）
    std::unique_ptr<ServiceRegistry> service_registry_;

    std::atomic<bool> running_{false};
    std::string instance_name_;
    std::string config_file_;
    std::string topic_map_;

    bool parseArgs(int argc, char* argv[]);
    void signalLoop();
};
