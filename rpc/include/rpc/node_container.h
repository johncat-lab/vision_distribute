#pragma once
#include "rpc/node_base.h"
#include "rpc/node_factory.h"
#include "rpc/edge_manager.h"
#include "rpc/service_endpoint_registry.h"
#include "rpc/message_types.h"
#include <memory>
#include <string>
#include <atomic>

/// @brief 节点容器：管理一个 NodeBase 的完整生命周期。
/// 职责：
/// - 解析命令行参数
/// - 创建传输层（NodeFactory）
/// - 调用节点的 initDataflow / initServices / start
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

private:
    std::unique_ptr<NodeBase> node_;
    std::unique_ptr<NodeFactory> factory_;
    std::unique_ptr<NodeEdgeManager> edges_;
    std::unique_ptr<ServiceEndpointRegistry> services_;

    std::atomic<bool> running_{false};
    std::string instance_name_;
    std::string config_file_;
    std::string topic_map_;

    bool parseArgs(int argc, char* argv[]);
    void signalLoop();
};
