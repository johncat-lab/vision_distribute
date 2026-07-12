#pragma once
#include "rpc/node_base.h"
#include "rpc/node_container.h"
#include "rpc/message_types.h"
#include <icamera.h>           // 相机抽象接口
#include <camera_factory.h>    // 相机工厂
#include <string>
#include <memory>
#include <mutex>
#include <atomic>

/// @brief 相机节点：
/// - 数据流通道：输出 FrameMsg 到 "frame_output" 端口
/// - 服务通道：提供 camera 角色（set_exposure/set_gain/get_config 等）
class CameraNode : public NodeBase {
public:
    CameraNode() = default;
    ~CameraNode() override;

    // ========== 声明式描述 ==========
    NodeManifest describe() const override;

    // ========== 数据流通道初始化 ==========
    void initDataflow(NodeEdgeManager& edges,
                       const std::string& config_file) override;

    // ========== 服务通道初始化 ==========
    void initServices(ServiceEndpointRegistry& services) override;
    void initServices(ServiceEndpointRegistry& services, NodeContainer& container) override;

    // ========== 生命周期 ==========
    bool start() override;
    void stop() override;
    void tick(std::atomic<bool>& running) override;

private:
    // 相机配置结构体
    struct CameraConfig {
        std::string camera_type = "hik";  // 相机类型 ("hik", "basler", ...)
        int camera_index = 0;
        std::string trigger_mode = "continuous";
        std::string pixel_format = "Mono8";
        std::string exposure_auto = "continuous";
        float exposure_time = 10000.0f;
        std::string gain_auto = "continuous";
        float gain = 0.0f;
        float frame_rate = 30.0f;
    };

    // 从 XML 加载
    static CameraConfig loadCameraConfig(const std::string& path);

    // 相机与数据访问
    std::unique_ptr<ICamera> camera_;  // 使用抽象接口，通过工厂创建
    std::atomic<bool> camera_ready_{false};
    std::mutex camera_mutex_;
    CameraConfig cam_cfg_;

    // 数据流：发布者
    std::shared_ptr<IPublisher<FrameMsg>> frame_pub_;

    // 辅助：端点处理函数
    ServiceResponse handleSetExposure(const ServiceRequest& req);
    ServiceResponse handleSetGain(const ServiceRequest& req);
    ServiceResponse handleSetTriggerMode(const ServiceRequest& req);
    ServiceResponse handleSoftTrigger(const ServiceRequest& req);
    ServiceResponse handleGetConfig(const ServiceRequest& req);
};
