#pragma once
#include "rpc/node_base.h"
#include "rpc/node_container.h"
#include "rpc/message_types.h"
#include "detector.h"
#include "object_info.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <string>

/// @brief 检测器节点：
/// - 数据流通道：输入 FrameMsg，输出 DetectionMsg + AnnotationMsg
/// - 服务通道：提供 detector 角色（get_result/get_config/onoff/set_threshold 等）
class DetectorNode : public NodeBase {
public:
    DetectorNode() = default;
    ~DetectorNode() override;

    NodeManifest describe() const override;
    void initDataflow(NodeEdgeManager& edges,
                       const std::string& config_file) override;
    void initServices(ServiceEndpointRegistry& services) override;
    void initServices(ServiceEndpointRegistry& services, NodeContainer& container) override;

    bool start() override;
    void stop() override;
    void tick(std::atomic<bool>& running) override;

private:
    struct DetectorConfig {
        std::string detector = "opencv";
        std::string template_dir = "./template";
        float match_threshold = 0.55f;
        std::string segment_mode = "value";
        int v_threshold = 50;
        int grad_threshold = 30;
        int roi_y_center = -1;
        int roi_y_margin = -1;
        std::string model_path = "./models/yolov11_obb.onnx";
        std::string bg_ref_path;
        bool verify_with_template = true;
        bool enabled = false;
    };

    static DetectorConfig loadConfig(const std::string& path);

    // 检测器
    std::unique_ptr<Detector> detector_;
    std::mutex detector_mutex_;
    std::atomic<bool> detector_ready_{false};
    std::atomic<bool> detector_enabled_{false};
    
    // 配置文件路径
    std::string detector_config_file_;

    // 最新检测结果
    DetectionMsg latest_detection_;
    std::mutex detection_mutex_;

    // 数据流：订阅/发布
    std::shared_ptr<ISubscriber<FrameMsg>> frame_sub_;
    std::shared_ptr<IPublisher<DetectionMsg>> detection_pub_;
    std::shared_ptr<IPublisher<AnnotationMsg>> annotation_pub_;

    // 处理一帧（在回调中调用）
    void processFrame(const FrameMsg& frame);

    // 服务端点处理
    ServiceResponse handleGetResult(const ServiceRequest& req);
    ServiceResponse handleGetConfig(const ServiceRequest& req);
    ServiceResponse handleOnOff(const ServiceRequest& req);
    ServiceResponse handleSetThreshold(const ServiceRequest& req);
    ServiceResponse handleReloadTemplate(const ServiceRequest& req);
    ServiceResponse handleSetVThreshold(const ServiceRequest& req);
    ServiceResponse handleSetGradThreshold(const ServiceRequest& req);
    ServiceResponse handleSetSegmentMode(const ServiceRequest& req);
};
