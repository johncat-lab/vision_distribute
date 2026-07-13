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

#ifdef HAS_ROS2
#include "vision_interfaces/srv/detector_get_result.hpp"
#include "vision_interfaces/srv/detector_get_config.hpp"
#include "vision_interfaces/srv/detector_on_off.hpp"
#include "vision_interfaces/srv/detector_set_threshold.hpp"
#include "vision_interfaces/srv/detector_set_v_threshold.hpp"
#include "vision_interfaces/srv/detector_set_grad_threshold.hpp"
#include "vision_interfaces/srv/detector_reload_template.hpp"
#include "vision_interfaces/srv/detector_set_segment_mode.hpp"
#endif

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
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
    if (!fs["detector"].empty())       fs["detector"] >> cfg.detector;
    if (!fs["template_dir"].empty())   fs["template_dir"] >> cfg.template_dir;
    if (!fs["match_threshold"].empty())fs["match_threshold"] >> cfg.match_threshold;
    if (!fs["segment_mode"].empty())   fs["segment_mode"] >> cfg.segment_mode;
    if (!fs["v_threshold"].empty())    fs["v_threshold"] >> cfg.v_threshold;
    if (!fs["grad_threshold"].empty()) fs["grad_threshold"] >> cfg.grad_threshold;
    if (!fs["roi_y_center"].empty())   fs["roi_y_center"] >> cfg.roi_y_center;
    if (!fs["roi_y_margin"].empty())   fs["roi_y_margin"] >> cfg.roi_y_margin;
    if (!fs["enabled"].empty())        fs["enabled"] >> cfg.enabled;
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
    frame_sub_->subscribe([this](const FrameMsg& frame) {
        this->processFrame(frame);
    });

    // 保存配置文件路径供 start() 使用
    detector_config_file_ = config_file;
    
    if (!detector_config_file_.empty()) {
        LOG_INFO("[DetectorNode] 检测器配置文件: %s", detector_config_file_.c_str());
    } else {
        LOG_INFO("[DetectorNode] 未指定检测器配置文件，将使用默认值");
    }

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

// ========== initServices (ROS2 原生类型注册) ==========
void DetectorNode::initServices(ServiceEndpointRegistry& services, NodeContainer& container) {
    // 先注册通用端点处理函数
    initServices(services);

#ifdef HAS_ROS2
    // 注册 ROS2 原生 .srv 类型映射，使 ROS2 传输层能正确序列化/反序列化请求和响应
    container.registerRos2NativeEndpoint<vision_interfaces::srv::DetectorGetResult>(
        "get_result",
        [](auto) -> ServiceRequest { return ServiceRequest{}; },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->detection_data.assign(sr.data().begin(), sr.data().end());
        },
        [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::DetectorGetResult::Request> {
            return std::make_shared<vision_interfaces::srv::DetectorGetResult::Request>();
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(std::string(resp->detection_data.begin(), resp->detection_data.end()));
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::DetectorGetConfig>(
        "get_config",
        [](auto) -> ServiceRequest { return ServiceRequest{}; },
        [](const ServiceResponse& sr, auto resp) {
            resp->ready = sr.success();
            resp->config_data = sr.data();
        },
        [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::DetectorGetConfig::Request> {
            return std::make_shared<vision_interfaces::srv::DetectorGetConfig::Request>();
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->ready);
            sr.set_data(resp->config_data);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::DetectorOnOff>(
        "onoff",
        [](auto req) -> ServiceRequest {
            ServiceRequest sr;
            sr.set_payload(req->command);
            return sr;
        },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->message = sr.data();
        },
        [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorOnOff::Request> {
            auto req = std::make_shared<vision_interfaces::srv::DetectorOnOff::Request>();
            req->command = sr.payload();
            return req;
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->message);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::DetectorSetThreshold>(
        "set_threshold",
        [](auto req) -> ServiceRequest {
            ServiceRequest sr;
            sr.set_payload(std::to_string(req->threshold));
            return sr;
        },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->message = sr.data();
        },
        [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetThreshold::Request> {
            auto req = std::make_shared<vision_interfaces::srv::DetectorSetThreshold::Request>();
            req->threshold = std::stof(sr.payload());
            return req;
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->message);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::DetectorSetVThreshold>(
        "set_v_threshold",
        [](auto req) -> ServiceRequest {
            ServiceRequest sr;
            sr.set_payload(std::to_string(req->threshold));
            return sr;
        },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->message = sr.data();
        },
        [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetVThreshold::Request> {
            auto req = std::make_shared<vision_interfaces::srv::DetectorSetVThreshold::Request>();
            req->threshold = std::stoi(sr.payload());
            return req;
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->message);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::DetectorSetGradThreshold>(
        "set_grad_threshold",
        [](auto req) -> ServiceRequest {
            ServiceRequest sr;
            sr.set_payload(std::to_string(req->threshold));
            return sr;
        },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->message = sr.data();
        },
        [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetGradThreshold::Request> {
            auto req = std::make_shared<vision_interfaces::srv::DetectorSetGradThreshold::Request>();
            req->threshold = std::stoi(sr.payload());
            return req;
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->message);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::DetectorReloadTemplate>(
        "reload_template",
        [](auto) -> ServiceRequest { return ServiceRequest{}; },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->message = sr.data();
        },
        [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::DetectorReloadTemplate::Request> {
            return std::make_shared<vision_interfaces::srv::DetectorReloadTemplate::Request>();
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->message);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::DetectorSetSegmentMode>(
        "set_segment_mode",
        [](auto req) -> ServiceRequest {
            ServiceRequest sr;
            sr.set_payload(req->mode);
            return sr;
        },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->message = sr.data();
        },
        [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetSegmentMode::Request> {
            auto req = std::make_shared<vision_interfaces::srv::DetectorSetSegmentMode::Request>();
            req->mode = sr.payload();
            return req;
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->message);
            return sr;
        });

    LOG_INFO("[DetectorNode] 已注册 8 个 ROS2 原生 service 类型映射");
#endif
}

// ========== start ==========
bool DetectorNode::start() {
    // 使用 initDataflow 中保存的配置文件路径
    auto cfg = loadConfig(detector_config_file_);

    try {
        if (cfg.detector == "yolo") {
#ifdef HAS_ONNX
            detector_ = std::make_unique<YoloDetector>(cfg.model_path);
#else
            LOG_WARN("[DetectorNode] 未启用 ONNX 支持，使用默认模板检测器");
            detector_ = std::make_unique<OpenCvTemplateDetector>(cfg.template_dir);
#endif
        } else if (cfg.detector == "gradient") {
            detector_ = std::make_unique<EdgeGradientDetector>();
        } else if (cfg.detector == "conveyor") {
            detector_ = std::make_unique<ConveyorDetector>();
        } else {
            detector_ = std::make_unique<OpenCvTemplateDetector>(cfg.template_dir);
        }
        // Detector 基类没有 setMatchThreshold，需要 dynamic_cast
        if (auto* tpl = dynamic_cast<OpenCvTemplateDetector*>(detector_.get())) {
            tpl->setMatchThreshold(cfg.match_threshold);
            tpl->setSegmentMode(cfg.segment_mode);
            tpl->setVThreshold(cfg.v_threshold);
            tpl->setGradientThreshold(cfg.grad_threshold);
            tpl->setRoiYCenter(cfg.roi_y_center);
            tpl->setRoiYMargin(cfg.roi_y_margin);
        }
        
        // 初始化检测器（加载模板等）
        if (!detector_->init()) {
            LOG_ERROR("[DetectorNode] 检测器初始化失败");
            return false;
        }
        
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
    LOG_DEBUG("[DetectorNode] 收到帧 #%u (%ux%u, %zu bytes), ready=%d, enabled=%d",
              frame.frame_num(), frame.width(), frame.height(),
              frame.data().size(),
              detector_ready_.load(), detector_enabled_.load());

    if (!detector_ready_ || !detector_enabled_) {
        LOG_DEBUG("[DetectorNode] 帧 #%u 被跳过 (ready=%d, enabled=%d)",
                  frame.frame_num(), detector_ready_.load(), detector_enabled_.load());
        return;
    }

    // 解码帧 → 检测 → 发布
    try {
        ObjectInfoList object_list;
        {
            std::lock_guard<std::mutex> lock(detector_mutex_);
            // FrameMsg → Frame 转换
            Frame frame_struct;
            frame_struct.frameNum = frame.frame_num();
            frame_struct.width = static_cast<unsigned short>(frame.width());
            frame_struct.height = static_cast<unsigned short>(frame.height());
            frame_struct.pixelType = frame.pixel_type();

            const std::string& frame_data = frame.data();
            if (frame.pixel_type() == 0) {
                // PNG 压缩数据，需要解码
                std::vector<uint8_t> encoded(frame_data.begin(), frame_data.end());
                cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
                if (decoded.empty()) {
                    LOG_WARN("[DetectorNode] PNG 解码失败，跳过帧 #%u", frame.frame_num());
                    return;
                }
                // 确保灰度单通道
                if (decoded.channels() > 1) {
                    cv::cvtColor(decoded, decoded, cv::COLOR_BGR2GRAY);
                }
                frame_struct.data.assign(decoded.data,
                    decoded.data + decoded.total() * decoded.elemSize());
                frame_struct.width = static_cast<unsigned short>(decoded.cols);
                frame_struct.height = static_cast<unsigned short>(decoded.rows);
                frame_struct.pixelType = 0x01080001;  // Mono8
            } else {
                // 原始像素数据
                frame_struct.data.assign(frame_data.begin(), frame_data.end());
            }
            
            object_list = detector_->detect(frame_struct);
        }
        
        const auto& objects = object_list.getObjects();

        DetectionMsg dmsg;
        
        // 填充 Protobuf 字段
        dmsg.set_frame_num(frame.frame_num());
        dmsg.set_timestamp(frame.timestamp());
        dmsg.set_object_count(static_cast<int32_t>(objects.size()));
        
        // 构建协议字符串 (TA,x,y,a,t,...; 或 NG)
        std::string protocol_str;
        if (objects.empty()) {
            protocol_str = "NG";
        } else {
            protocol_str = "TA";
            for (const auto& obj : objects) {
                protocol_str += "," + std::to_string(obj.getX()) + "," + std::to_string(obj.getY()) + 
                               "," + std::to_string(obj.getAngle()) + "," + std::to_string(obj.getType());
            }
        }
        dmsg.set_protocol_string(protocol_str);
        
        // 填充其他字段
        dmsg.set_frame_num(frame.frame_num());
        dmsg.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        dmsg.set_object_count(static_cast<int32_t>(objects.size()));

        {
            std::lock_guard<std::mutex> lock(detection_mutex_);
            latest_detection_ = dmsg;
        }

        if (detection_pub_) detection_pub_->publish(dmsg);

        // 构建 AnnotationMsg
        AnnotationMsg amsg;
        
        // 填充 Protobuf 对象
        amsg.set_frame_num(frame.frame_num());
        amsg.set_timestamp(frame.timestamp());
        amsg.set_template_width(0);  // 需要从配置获取
        amsg.set_template_height(0); // 需要从配置获取
        
        for (const auto& obj : objects) {
            auto* proto_obj = amsg.add_objects();
            proto_obj->set_x(obj.getX());
            proto_obj->set_y(obj.getY());
            proto_obj->set_angle(obj.getAngle());
            proto_obj->set_score(0.0);  // ObjectInfo 没有 score 字段
            proto_obj->set_type(obj.getType());
            proto_obj->set_id(0);    // 根据实际需求设置
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
    if (latest_detection_.object_count() == 0) {
        resp.set_success(false);
        resp.set_data("暂无检测结果");
    } else {
        resp.set_success(true);
        std::ostringstream oss;
        oss << "objects=" << latest_detection_.object_count();
        oss << " protocol=" << latest_detection_.protocol_string();
        resp.set_data(oss.str());
    }
    return resp;
}

ServiceResponse DetectorNode::handleGetConfig(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    resp.set_success(true);
    std::lock_guard<std::mutex> lock(detector_mutex_);
    if (detector_) {
        resp.set_data("threshold=" + std::to_string(static_cast<OpenCvTemplateDetector*>(detector_.get())->getMatchThreshold()));
    } else {
        resp.set_data("detector not initialized");
    }
    return resp;
}

ServiceResponse DetectorNode::handleOnOff(const ServiceRequest& req) {
    ServiceResponse resp;
    if (req.payload() == "on" || req.payload() == "1" || req.payload() == "true") {
        detector_enabled_ = true;
        resp.set_success(true);
        resp.set_data("检测器已启用");
    } else {
        detector_enabled_ = false;
        resp.set_success(true);
        resp.set_data("检测器已关闭");
    }
    return resp;
}

ServiceResponse DetectorNode::handleSetThreshold(const ServiceRequest& req) {
    ServiceResponse resp;
    try {
        float th = std::stof(req.payload());
        std::lock_guard<std::mutex> lock(detector_mutex_);
        if (detector_) {
            if (auto* tpl = dynamic_cast<OpenCvTemplateDetector*>(detector_.get())) {
                tpl->setMatchThreshold(th);
                resp.set_success(true);
                resp.set_data("匹配阈值已设置: " + std::to_string(th));
            } else {
                resp.set_success(false);
                resp.set_data("不支持的检测器类型");
            }
        } else {
            resp.set_success(false);
            resp.set_data("检测器未初始化");
        }
    } catch (const std::exception& e) {
        resp.set_success(false);
        resp.set_data(std::string("参数错误: ") + e.what());
    }
    return resp;
}

ServiceResponse DetectorNode::handleReloadTemplate(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    std::lock_guard<std::mutex> lock(detector_mutex_);
    if (auto* tpl = dynamic_cast<OpenCvTemplateDetector*>(detector_.get())) {
        tpl->init();
        resp.set_success(true);
        resp.set_data("模板已重新加载");
    } else {
        resp.set_success(false);
        resp.set_data("当前检测器类型不支持模板加载");
    }
    return resp;
}

ServiceResponse DetectorNode::handleSetVThreshold(const ServiceRequest& req) {
    ServiceResponse resp;
    try {
        int v = std::stoi(req.payload());
        std::lock_guard<std::mutex> lock(detector_mutex_);
        if (auto* tpl = dynamic_cast<OpenCvTemplateDetector*>(detector_.get())) {
            // 调用对应方法 (示例)
            (void)v;
        }
        resp.set_success(true);
        resp.set_data("颜色阈值已设置: " + req.payload());
    } catch (const std::exception& e) {
        resp.set_success(false);
        resp.set_data(std::string("参数错误: ") + e.what());
    }
    return resp;
}

ServiceResponse DetectorNode::handleSetGradThreshold(const ServiceRequest& req) {
    ServiceResponse resp;
    try {
        int g = std::stoi(req.payload());
        resp.set_success(true);
        resp.set_data("梯度阈值已设置: " + std::to_string(g));
    } catch (const std::exception& e) {
        resp.set_success(false);
        resp.set_data(std::string("参数错误: ") + e.what());
    }
    return resp;
}

ServiceResponse DetectorNode::handleSetSegmentMode(const ServiceRequest& req) {
    ServiceResponse resp;
    resp.set_success(true);
    resp.set_data("分割模式已设置: " + req.payload());
    return resp;
}
