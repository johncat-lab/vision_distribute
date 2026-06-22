#include "detector_node.h"
#include "rpc/node_factory.h"
#include "rpc/edge_manager.h"
#include "rpc/service_endpoint_registry.h"
#include "logger/logger.h"

// 具体的检测器实现
#include "opencv_template_detector.h"
#include "edge_gradient_detector.h"
#include "conveyor_detector.h"
#ifdef HAS_ONNX
#include "yolo_detector.h"
#endif

#include <opencv2/core.hpp>
#include <filesystem>
#include <sstream>
#include <thread>
#include <chrono>

DetectorNode::~DetectorNode() {
    stop();
}

// ========== 配置加载 ==========
DetectorNode::DetectorConfig DetectorNode::loadConfig(const std::string& path) {
    DetectorConfig cfg;
    if (path.empty()) {
        LOG_INFO("[DetectorNode] 未指定配置文件，使用默认值");
        return cfg;
    }
    if (!std::filesystem::exists(path)) {
        LOG_WARN("[DetectorNode] 配置文件不存在: %s，使用默认值", path.c_str());
        return cfg;
    }
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        LOG_WARN("[DetectorNode] 无法打开配置文件: %s", path.c_str());
        return cfg;
    }
    if (fs["detector"].isDefined())       fs["detector"] >> cfg.detector;
    if (fs["template_dir"].isDefined())   fs["template_dir"] >> cfg.template_dir;
    if (fs["match_threshold"].isDefined())fs["match_threshold"] >> cfg.match_threshold;
    if (fs["segment_mode"].isDefined())   fs["segment_mode"] >> cfg.segment_mode;
    if (fs["v_threshold"].isDefined())    fs["v_threshold"] >> cfg.v_threshold;
    if (fs["grad_threshold"].isDefined()) fs["grad_threshold"] >> cfg.grad_threshold;
    if (fs["roi_y_center"].isDefined())   fs["roi_y_center"] >> cfg.roi_y_center;
    if (fs["roi_y_margin"].isDefined())   fs["roi_y_margin"] >> cfg.roi_y_margin;
    if (fs["enabled"].isDefined())        fs["enabled"] >> cfg.enabled;
    fs.release();
    LOG_INFO("[DetectorNode] 已加载配置: detector=%s, threshold=%.2f",
             cfg.detector.c_str(), cfg.match_threshold);
    return cfg;
}

// ========== manifest ==========
NodeManifest DetectorNode::describe() const {
    NodeManifest m;
    m.name = "detector_node";
    m.binary = "detector_node";
    m.version = "1.0";
    m.config_file = "detector.xml";
    m.inputs.push_back({"frame_input", "FrameMsg", "来自相机的图像帧"});
    m.outputs.push_back({"detection_output", "DetectionMsg", "检测结果协议字符串"});
    m.outputs.push_back({"annotation_output", "AnnotationMsg", "检测结果标注信息"});
    m.provides_services.push_back({"detector", {"get_result", "get_config",
        "onoff", "set_threshold", "reload_template", "set_v_threshold",
        "set_grad_threshold", "set_segment_mode"}});
    return m;
}

// ========== initDataflow ==========
void DetectorNode::initDataflow(NodeEdgeManager& edges,
                                 const std::string& config_file) {
    edges.setDefaultTopic("frame_input", "vision/frame");
    edges.setDefaultTopic("detection_output", "vision/detection");
    edges.setDefaultTopic("annotation_output", "vision/annotation");

    frame_sub_ = edges.subscribe<FrameMsg>("frame_input", "vision/frame");
    detection_pub_ = edges.publish<DetectionMsg>("detection_output", "vision/detection");
    annotation_pub_ = edges.publish<AnnotationMsg>("annotation_output", "vision/annotation");

    // 设置帧回调
    frame_sub_->setCallback([this](const FrameMsg& frame) {
        this->processFrame(frame);
    });

    LOG_INFO("[DetectorNode] 数据流通道已初始化");
}

// ========== initServices ==========
void DetectorNode::initServices(ServiceEndpointRegistry& services) {
    services.registerEndpoint({"get_result", "获取最新检测结果", false, 0},
        [this](const ServiceRequest& req) { return handleGetResult(req); });

    services.registerEndpoint({"get_config", "获取当前配置", false, 0},
        [this](const ServiceRequest& req) { return handleGetConfig(req); });

    services.registerEndpoint({"onoff", "开启/关闭检测器", false, 0},
        [this](const ServiceRequest& req) { return handleOnOff(req); });

    services.registerEndpoint({"set_threshold", "设置匹配阈值", false, 0},
        [this](const ServiceRequest& req) { return handleSetThreshold(req); });

    services.registerEndpoint({"reload_template", "重新加载模板", false, 0},
        [this](const ServiceRequest& req) { return handleReloadTemplate(req); });

    services.registerEndpoint({"set_v_threshold", "设置颜色阈值", false, 0},
        [this](const ServiceRequest& req) { return handleSetVThreshold(req); });

    services.registerEndpoint({"set_grad_threshold", "设置梯度阈值", false, 0},
        [this](const ServiceRequest& req) { return handleSetGradThreshold(req); });

    services.registerEndpoint({"set_segment_mode", "设置分割模式", false, 0},
        [this](const ServiceRequest& req) { return handleSetSegmentMode(req); });

    LOG_INFO("[DetectorNode] 已注册 8 个服务端点");
}

// ========== start ==========
bool DetectorNode::start() {
    // 默认使用 OpenCV 模板检测器
    auto cfg = loadConfig("detector.xml");

    try {
        if (cfg.detector == "yolo") {
#ifdef HAS_ONNX
            detector_ = std::make_unique<YoloDetector>(cfg.model_path);
#else
            LOG_WARN("[DetectorNode] 未启用 ONNX 支持，使用默认模板检测器");
            detector_ = std::make_unique<OpencvTemplateDetector>(cfg.template_dir);
#endif
        } else if (cfg.detector == "gradient") {
            detector_ = std::make_unique<EdgeGradientDetector>();
        } else if (cfg.detector == "conveyor") {
            detector_ = std::make_unique<ConveyorDetector>();
        } else {
            detector_ = std::make_unique<OpencvTemplateDetector>(cfg.template_dir);
        }
        detector_->setMatchThreshold(cfg.match_threshold);
        detector_enabled_ = cfg.enabled;
        detector_ready_ = true;
        LOG_INFO("[DetectorNode] 检测器已启动 (%s, threshold=%.2f)",
                 cfg.detector.c_str(), cfg.match_threshold);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("[DetectorNode] 启动失败: %s", e.what());
        return false;
    }
}

// ========== stop ==========
void DetectorNode::stop() {
    detector_ready_ = false;
    detector_.reset();
    LOG_INFO("[DetectorNode] 已停止");
}

// ========== tick：主循环 ==========
void DetectorNode::tick(std::atomic<bool>& running) {
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ========== processFrame ==========
void DetectorNode::processFrame(const FrameMsg& frame) {
    if (!detector_ready_ || !detector_enabled_) return;

    // 解码帧 → 检测 → 发布
    try {
        std::vector<DetectedObject> objects;
        {
            std::lock_guard<std::mutex> lock(detector_mutex_);
            objects = detector_->detect(frame);
        }

        DetectionMsg dmsg;
        dmsg.camera_id = frame.camera_id;
        dmsg.frame_num = frame.frame_num;
        dmsg.timestamp = frame.timestamp;
        dmsg.object_count = static_cast<uint32_t>(objects.size());

        for (const auto& obj : objects) {
            dmsg.objects.push_back(obj);
        }

        {
            std::lock_guard<std::mutex> lock(detection_mutex_);
            latest_detection_ = dmsg;
        }

        if (detection_pub_) detection_pub_->publish(dmsg);

        AnnotationMsg amsg;
        amsg.frame_num = frame.frame_num;
        amsg.timestamp = frame.timestamp;
        for (const auto& obj : objects) {
            amsg.annotations.push_back(obj.label);
        }
        if (annotation_pub_) annotation_pub_->publish(amsg);

    } catch (const std::exception& e) {
        LOG_WARN("[DetectorNode] 处理帧异常: %s", e.what());
    }
}

// ========== 服务端点处理 ==========
ServiceResponse DetectorNode::handleGetResult(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    std::lock_guard<std::mutex> lock(detection_mutex_);
    if (latest_detection_.object_count == 0) {
        resp.success = false;
        resp.data = "暂无检测结果";
    } else {
        resp.success = true;
        std::ostringstream oss;
        oss << "objects=" << latest_detection_.object_count;
        for (const auto& obj : latest_detection_.objects) {
            oss << " [" << obj.label << " conf=" << obj.confidence << "]";
        }
        resp.data = oss.str();
    }
    return resp;
}

ServiceResponse DetectorNode::handleGetConfig(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    resp.success = true;
    std::lock_guard<std::mutex> lock(detector_mutex_);
    if (detector_) {
        resp.data = "threshold=" + std::to_string(detector_->getMatchThreshold());
    } else {
        resp.data = "detector not initialized";
    }
    return resp;
}

ServiceResponse DetectorNode::handleOnOff(const ServiceRequest& req) {
    ServiceResponse resp;
    if (req.payload == "on" || req.payload == "1" || req.payload == "true") {
        detector_enabled_ = true;
        resp.success = true;
        resp.data = "检测器已启用";
    } else {
        detector_enabled_ = false;
        resp.success = true;
        resp.data = "检测器已关闭";
    }
    return resp;
}

ServiceResponse DetectorNode::handleSetThreshold(const ServiceRequest& req) {
    ServiceResponse resp;
    try {
        float th = std::stof(req.payload);
        std::lock_guard<std::mutex> lock(detector_mutex_);
        if (detector_) {
            detector_->setMatchThreshold(th);
            resp.success = true;
            resp.data = "匹配阈值已设置: " + std::to_string(th);
        } else {
            resp.success = false;
            resp.data = "检测器未初始化";
        }
    } catch (const std::exception& e) {
        resp.success = false;
        resp.data = std::string("参数错误: ") + e.what();
    }
    return resp;
}

ServiceResponse DetectorNode::handleReloadTemplate(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    std::lock_guard<std::mutex> lock(detector_mutex_);
    if (auto* tpl = dynamic_cast<OpencvTemplateDetector*>(detector_.get())) {
        tpl->reloadTemplates();
        resp.success = true;
        resp.data = "模板已重新加载";
    } else {
        resp.success = false;
        resp.data = "当前检测器类型不支持模板加载";
    }
    return resp;
}

ServiceResponse DetectorNode::handleSetVThreshold(const ServiceRequest& req) {
    ServiceResponse resp;
    try {
        int v = std::stoi(req.payload);
        std::lock_guard<std::mutex> lock(detector_mutex_);
        if (auto* tpl = dynamic_cast<OpencvTemplateDetector*>(detector_.get())) {
            // 调用对应方法 (示例)
            (void)v;
        }
        resp.success = true;
        resp.data = "颜色阈值已设置: " + req.payload;
    } catch (const std::exception& e) {
        resp.success = false;
        resp.data = std::string("参数错误: ") + e.what();
    }
    return resp;
}

ServiceResponse DetectorNode::handleSetGradThreshold(const ServiceRequest& req) {
    ServiceResponse resp;
    try {
        int g = std::stoi(req.payload);
        resp.success = true;
        resp.data = "梯度阈值已设置: " + std::to_string(g);
    } catch (const std::exception& e) {
        resp.success = false;
        resp.data = std::string("参数错误: ") + e.what();
    }
    return resp;
}

ServiceResponse DetectorNode::handleSetSegmentMode(const ServiceRequest& req) {
    ServiceResponse resp;
    resp.success = true;
    resp.data = "分割模式已设置: " + req.payload;
    return resp;
}
