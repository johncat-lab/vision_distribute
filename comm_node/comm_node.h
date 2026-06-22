#pragma once
#include "rpc/node_base.h"
#include "rpc/node_container.h"
#include "rpc/message_types.h"
#include "vision_server.h"
#include "tcp_client.h"
#include "object_info.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>

/// @brief 通信节点：
/// - 数据流通道：输入 DetectionMsg，转发到外部 TCP 客户端或服务器
/// - 服务通道：提供 comm 角色（set_config/get_config/get_status）
class CommNode : public NodeBase {
public:
    CommNode() = default;
    ~CommNode() override;

    NodeManifest describe() const override;
    void initDataflow(NodeEdgeManager& edges,
                       const std::string& config_file) override;
    void initServices(ServiceEndpointRegistry& services) override;
    void initServices(ServiceEndpointRegistry& services, NodeContainer& container) override;

    bool start() override;
    void stop() override;
    void tick(std::atomic<bool>& running) override;

private:
    struct CommConfig {
        std::string mode = "server";
        std::string host = "0.0.0.0";
        int port = 7930;
        int server_mode = 2;
        int interval_ms = 100;
    };

    static CommConfig loadConfig(const std::string& path);
    static bool saveConfig(const std::string& path, const CommConfig& cfg);

    // 通信后端
    std::unique_ptr<VisionServer> server_;
    std::unique_ptr<TcpClient> client_;
    std::mutex comm_mutex_;
    CommConfig cfg_;

    // 最近收到的检测结果
    DetectionMsg latest_detection_;
    std::mutex detection_mutex_;

    // 数据流
    std::shared_ptr<ISubscriber<DetectionMsg>> detection_sub_;

    ServiceResponse handleSetConfig(const ServiceRequest& req);
    ServiceResponse handleGetConfig(const ServiceRequest& req);
    ServiceResponse handleGetStatus(const ServiceRequest& req);
};
