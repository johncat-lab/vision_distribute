#include "rpc/node_factory.h"
#include "rpc/config_loader.h"
#include "rpc/message_types.h"
#include "rpc/node_manifest.h"
#include "rpc/edge_manager.h"

#include "detector.h"
#include "opencv_template_detector.h"
#include "edge_gradient_detector.h"
#include "conveyor_detector.h"
#ifdef HAS_ONNX
#include "yolo_detector.h"
#endif
#include "object_info.h"

#include "logger/logger.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <memory>
#include <csignal>
#include <sstream>
#include <filesystem>
#include <opencv2/core.hpp>

#ifdef HAS_ROS2
#include "ros2_backend.h"
#include "vision_interfaces/srv/detector_get_config.hpp"
#include "vision_interfaces/srv/detector_get_result.hpp"
#include "vision_interfaces/srv/detector_on_off.hpp"
#include "vision_interfaces/srv/detector_reload_template.hpp"
#include "vision_interfaces/srv/detector_set_grad_threshold.hpp"
#include "vision_interfaces/srv/detector_set_segment_mode.hpp"
#include "vision_interfaces/srv/detector_set_threshold.hpp"
#include "vision_interfaces/srv/detector_set_v_threshold.hpp"
#endif

// ========== 全局状态 ==========
static std::unique_ptr<Detector> g_detector;
static std::mutex g_detector_mutex;  // 保护 g_detector 的访问
static DetectionMsg g_latest_detection;
static std::mutex g_detection_mutex;
static std::string g_detector_config_summary;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_detector_ready{false};
static std::atomic<bool> g_detector_enabled{false};  // 检测器开关状态
static std::atomic<bool> g_processing_frame{false};   // 是否正在处理帧

// 全局发布者 (需要在回调中使用)
static std::shared_ptr<IPublisher<DetectionMsg>> g_detection_pub;
static std::shared_ptr<IPublisher<AnnotationMsg>> g_annotation_pub;
static std::mutex g_publisher_mutex;  // 保护发布者的访问

// ========== 信号处理 ==========
static void signalHandler(int sig) {
    (void)sig;
    LOG_INFO("收到退出信号，正在关闭...");
    g_running = false;
}

// ========== 构建 manifest ==========
static NodeManifest buildManifest() {
    NodeManifest m;
    m.name = "detector_node";
    m.binary = "detector_node";
    m.version = "1.0";
    m.config_file = "detector.xml";
    m.inputs.push_back({"frame_input", "FrameMsg", "来自相机的图像帧"});
    m.outputs.push_back({"detection_output", "DetectionMsg", "检测结果协议字符串"});
    m.outputs.push_back({"annotation_output", "AnnotationMsg", "检测结果标注信息"});
    m.provides_services.push_back({"detector", {"get_result", "get_config", "onoff", "set_threshold", "reload_template"}});
    return m;
}

// ========== 命令行用法 ==========
static void printUsage(const char* prog) {
    LOG_INFO("用法: %s --config <system_config.xml> --detector-config <detector.xml>", prog);
}

// ========== 检测器配置 ==========
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
    bool enabled = false;  // 检测器是否启用（默认关闭）
};

// 保存当前配置，供 reload_template 使用
static DetectorConfig g_det_cfg;

// ========== 加载检测器配置 ==========
static DetectorConfig loadDetectorConfig(const std::string& config_path) {
    DetectorConfig cfg;

    cv::FileStorage fs(config_path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        LOG_ERROR("无法打开检测器配置文件: %s", config_path.c_str());
        return cfg;
    }

    if (!fs["detector"].empty())         cfg.detector = (std::string)fs["detector"];
    if (!fs["template_dir"].empty())     cfg.template_dir = (std::string)fs["template_dir"];
    // 相对路径以配置文件所在目录为基准
    if (!cfg.template_dir.empty() && cfg.template_dir[0] != '/') {
        std::filesystem::path config_dir = std::filesystem::path(config_path).parent_path();
        cfg.template_dir = (config_dir / cfg.template_dir).string();
    }
    if (!fs["match_threshold"].empty())  cfg.match_threshold = (float)fs["match_threshold"];
    if (!fs["segment_mode"].empty())     cfg.segment_mode = (std::string)fs["segment_mode"];
    if (!fs["v_threshold"].empty())      cfg.v_threshold = (int)fs["v_threshold"];
    if (!fs["grad_threshold"].empty())   cfg.grad_threshold = (int)fs["grad_threshold"];
    {
        cv::FileNode n = fs["roi_y_center"];
        if (!n.empty() && n.isInt() && (int)n > 0) cfg.roi_y_center = (int)n;
    }
    {
        cv::FileNode n = fs["roi_y_margin"];
        if (!n.empty() && n.isInt() && (int)n > 0) cfg.roi_y_margin = (int)n;
    }
    if (!fs["model_path"].empty())       cfg.model_path = (std::string)fs["model_path"];
    if (!fs["bg_ref_path"].empty()) {
        std::string s = (std::string)fs["bg_ref_path"];
        if (!s.empty()) cfg.bg_ref_path = s;
    }
    if (!fs["verify_with_template"].empty()) cfg.verify_with_template = (int)fs["verify_with_template"] != 0;
    if (!fs["enabled"].empty()) cfg.enabled = (int)fs["enabled"] != 0;

    fs.release();
    return cfg;
}

// ========== 创建检测器实例 ==========
static std::unique_ptr<Detector> createDetector(const DetectorConfig& cfg) {
    if (cfg.detector == "opencv" || cfg.detector == "template") {
        auto* cv_det = new OpenCvTemplateDetector(cfg.template_dir, cfg.match_threshold);
        cv_det->setSegmentMode(cfg.segment_mode);
        if (cfg.segment_mode == "value") {
            cv_det->setVThreshold(cfg.v_threshold);
        } else if (cfg.segment_mode == "gradient") {
            cv_det->setGradientThreshold(cfg.grad_threshold);
        }
        return std::unique_ptr<Detector>(cv_det);
    } else if (cfg.detector == "edge_gradient") {
        auto* eg_det = new EdgeGradientDetector(cfg.template_dir, cfg.match_threshold);
        eg_det->setSegmentMode(cfg.segment_mode);
        if (cfg.segment_mode == "value") {
            eg_det->setVThreshold(cfg.v_threshold);
        } else if (cfg.segment_mode == "gradient") {
            eg_det->setGradientThreshold(cfg.grad_threshold);
        }
        if (cfg.roi_y_center >= 0) eg_det->setRoiYCenter(cfg.roi_y_center);
        if (cfg.roi_y_margin >= 0) eg_det->setRoiYMargin(cfg.roi_y_margin);
        return std::unique_ptr<Detector>(eg_det);
    } else if (cfg.detector == "conveyor") {
        auto* conv_det = new ConveyorDetector(cfg.template_dir, cfg.match_threshold);
        if (cfg.roi_y_center >= 0) conv_det->setRoiYCenter(cfg.roi_y_center);
        if (cfg.roi_y_margin >= 0) conv_det->setRoiYMargin(cfg.roi_y_margin);
        return std::unique_ptr<Detector>(conv_det);
    } else if (cfg.detector == "yolo") {
#ifdef HAS_ONNX
        return std::make_unique<YoloDetector>(cfg.model_path, 0.5f, 0.45f);
#else
        LOG_ERROR("YOLO 检测器需要 ONNX Runtime，请使用 -DUSE_ONNX=ON 重新编译");
        return nullptr;
#endif
    }

    LOG_ERROR("未知的检测器类型: %s", cfg.detector.c_str());
    return nullptr;
}

// ========== 生成配置摘要字符串 ==========
static std::string makeConfigSummary(const DetectorConfig& cfg) {
    std::ostringstream oss;
    oss << "detector=" << cfg.detector
        << ",template_dir=" << cfg.template_dir
        << ",match_threshold=" << cfg.match_threshold
        << ",segment_mode=" << cfg.segment_mode
        << ",v_threshold=" << cfg.v_threshold
        << ",grad_threshold=" << cfg.grad_threshold
        << ",roi_y_center=" << cfg.roi_y_center
        << ",roi_y_margin=" << cfg.roi_y_margin;
    if (cfg.detector == "yolo") {
#ifdef HAS_ONNX
        oss << ",model_path=" << cfg.model_path;
#endif
    }
    if (cfg.detector == "conveyor") {
        oss << ",bg_ref_path=" << cfg.bg_ref_path
            << ",verify_with_template=" << (cfg.verify_with_template ? 1 : 0);
    }
    return oss.str();
}

// ========== 获取当前时间戳 (毫秒) ==========
static uint64_t getTimestampMs() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

// ========== FrameMsg -> Frame 转换 ==========
static Frame frameMsgToFrame(const FrameMsg& msg) {
    Frame frame;
    frame.data = msg.data;
    frame.width = msg.width;
    frame.height = msg.height;
    frame.pixelType = msg.pixel_type;
    frame.frameNum = msg.frame_num;
    return frame;
}

// ========== 主函数 ==========
int main(int argc, char* argv[]) {
    std::string config_path;
    std::string detector_config_path;

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--describe") {
            std::cout << buildManifest().toJson() << std::endl;
            return 0;
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--detector-config" && i + 1 < argc) {
            detector_config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (config_path.empty()) {
        LOG_ERROR("未指定系统配置文件");
        printUsage(argv[0]);
        return 1;
    }

    if (detector_config_path.empty()) {
        LOG_ERROR("未指定检测器配置文件 (--detector-config)");
        printUsage(argv[0]);
        return 1;
    }

    // 加载系统配置
    NodeConfig config;
    try {
        config = ConfigLoader::loadSystemConfig(config_path);
        config.node_name = "detector_node";
    } catch (const std::exception& e) {
        LOG_ERROR("加载系统配置失败: %s", e.what());
        return 1;
    }

    std::string transport_name;
    switch (config.transport) {
    case TransportType::ZEROMQ: transport_name = "ZeroMQ"; break;
    case TransportType::ZENOH:  transport_name = "Zenoh"; break;
    case TransportType::ROS2:   transport_name = "ROS2"; break;
    }

    // 初始化日志系统
    vision::Logger::initFromXml(detector_config_path);
    // 设置 node 名称以创建子目录
    vision::Logger::setNodeName("detector_node");

    LOG_INFO("Detector Node 启动");
    LOG_INFO("传输方式: %s", transport_name.c_str());
    LOG_INFO("检测器配置文件: %s", detector_config_path.c_str());

    // 加载检测器配置
    DetectorConfig det_cfg = loadDetectorConfig(detector_config_path);
    LOG_INFO("检测器类型: %s", det_cfg.detector.c_str());
    LOG_INFO("模版目录: %s", det_cfg.template_dir.c_str());
    LOG_INFO("匹配阈值: %f", det_cfg.match_threshold);
    LOG_INFO("分割模式: %s", det_cfg.segment_mode.c_str());

    g_detector_config_summary = makeConfigSummary(det_cfg);
    g_det_cfg = det_cfg;
    
    // 设置检测器初始开关状态（从配置文件读取，默认关闭）
    g_detector_enabled = det_cfg.enabled;
    LOG_INFO("检测器初始状态: %s", g_detector_enabled.load() ? "启用" : "禁用");

    // 创建检测器实例（失败时不退出，以未就绪状态运行，等待 reload_template）
    g_detector = createDetector(det_cfg);
    if (!g_detector) {
        LOG_WARN("创建检测器失败，将以未就绪状态运行，等待 reload_template 命令");
        g_detector_ready = false;
    } else if (!g_detector->init()) {
        LOG_WARN("检测器初始化失败（模版可能未就绪），将以未就绪状态运行，等待 reload_template 命令");
        g_detector_ready = false;
    } else {
        g_detector_ready = true;
        LOG_INFO("检测器初始化完成");
    }

    // 注册信号处理
    signal(SIGINT, signalHandler);
#ifndef _WIN32
    signal(SIGTERM, signalHandler);
#endif

    // 创建 NodeFactory + EdgeManager
    NodeFactory factory(config);
    NodeEdgeManager edges(factory, "detector_node");
    edges.setDefaultTopic("frame_input", "vision/frame");
    edges.setDefaultTopic("detection_output", "vision/detection");
    edges.setDefaultTopic("annotation_output", "vision/annotation");
    edges.parseArgs(argc, argv);

    // 创建帧订阅者
    auto frame_sub = edges.subscribe<FrameMsg>("frame_input", "vision/frame");
    LOG_INFO("[Detector] 帧订阅者已创建，topic: %s", frame_sub->getTopic().c_str());

    // 创建检测发布者
    g_detection_pub = edges.publish<DetectionMsg>("detection_output", "vision/detection");
    LOG_INFO("[Detector] 检测发布者已创建，topic: %s", g_detection_pub->getTopic().c_str());

    // 创建标注发布者
    g_annotation_pub = edges.publish<AnnotationMsg>("annotation_output", "vision/annotation");
    LOG_INFO("[Detector] 标注发布者已创建，topic: %s", g_annotation_pub->getTopic().c_str());

    // 创建服务 (detector/get_result, detector/get_config)
    auto service = factory.createService<ServiceRequest, ServiceResponse>("detector");
    LOG_INFO("服务创建成功: detector");

#ifdef HAS_ROS2
    // 注册原生 ROS2 service 类型映射（双向转换：服务端+客户端）
    // serve() 会自动创建对应的原生 service，call() 也通过原生 client 调用
    if (auto* rs = dynamic_cast<Ros2Service<ServiceRequest, ServiceResponse>*>(service.get())) {
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorGetResult>(
            "get_result",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->detection_data.assign(sr.data.begin(), sr.data.end());
            },
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::DetectorGetResult::Request> {
                return std::make_shared<vision_interfaces::srv::DetectorGetResult::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data.assign(resp->detection_data.begin(), resp->detection_data.end());
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorGetConfig>(
            "get_config",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse& sr, auto resp) {
                resp->ready = sr.success;
                resp->config_data = sr.data;
            },
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::DetectorGetConfig::Request> {
                return std::make_shared<vision_interfaces::srv::DetectorGetConfig::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->ready;
                sr.data = resp->config_data;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorOnOff>(
            "onoff",
            [](auto req) -> ServiceRequest {
                ServiceRequest sr;
                sr.payload = req->command;
                return sr;
            },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorOnOff::Request> {
                auto req = std::make_shared<vision_interfaces::srv::DetectorOnOff::Request>();
                req->command = sr.payload;
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorSetThreshold>(
            "set_threshold",
            [](auto req) -> ServiceRequest {
                ServiceRequest sr;
                sr.payload = std::to_string(req->threshold);
                return sr;
            },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetThreshold::Request> {
                auto req = std::make_shared<vision_interfaces::srv::DetectorSetThreshold::Request>();
                req->threshold = std::stof(sr.payload);
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorSetVThreshold>(
            "set_v_threshold",
            [](auto req) -> ServiceRequest {
                ServiceRequest sr;
                sr.payload = std::to_string(req->threshold);
                return sr;
            },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetVThreshold::Request> {
                auto req = std::make_shared<vision_interfaces::srv::DetectorSetVThreshold::Request>();
                req->threshold = std::stoi(sr.payload);
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorSetGradThreshold>(
            "set_grad_threshold",
            [](auto req) -> ServiceRequest {
                ServiceRequest sr;
                sr.payload = std::to_string(req->threshold);
                return sr;
            },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetGradThreshold::Request> {
                auto req = std::make_shared<vision_interfaces::srv::DetectorSetGradThreshold::Request>();
                req->threshold = std::stoi(sr.payload);
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorReloadTemplate>(
            "reload_template",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::DetectorReloadTemplate::Request> {
                return std::make_shared<vision_interfaces::srv::DetectorReloadTemplate::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::DetectorSetSegmentMode>(
            "set_segment_mode",
            [](auto req) -> ServiceRequest {
                ServiceRequest sr;
                sr.payload = req->mode;
                return sr;
            },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::DetectorSetSegmentMode::Request> {
                auto req = std::make_shared<vision_interfaces::srv::DetectorSetSegmentMode::Request>();
                req->mode = sr.payload;
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
    }
#endif

    // 注册 get_result 服务处理函数
    service->serve("get_result", [](const ServiceRequest& req) -> ServiceResponse {
        (void)req;
        ServiceResponse resp;
        std::lock_guard<std::mutex> lock(g_detection_mutex);
        resp.success = true;
        resp.data = g_latest_detection.serialize();
        return resp;
    });

    // 注册 get_config 服务处理函数
    service->serve("get_config", [](const ServiceRequest& req) -> ServiceResponse {
        LOG_DEBUG("[get_config] 收到请求");
        ServiceResponse resp;
        resp.success = g_detector_ready.load();
        resp.data = g_detector_config_summary
                  + ",ready=" + (g_detector_ready.load() ? "1" : "0");
        LOG_DEBUG("[get_config] 返回结果: success=%d, data=%s", resp.success, resp.data.c_str());
        return resp;
    });

    // 注册 onoff 服务处理函数：控制检测器开关
    service->serve("onoff", [](const ServiceRequest& req) -> ServiceResponse {
        LOG_DEBUG("[onoff] 收到请求: %s", req.payload.c_str());
        ServiceResponse resp;
        std::string cmd = req.payload;
        
        if (cmd == "on") {
            g_detector_enabled = true;
            LOG_INFO("检测器已启用");
            resp.success = true;
            resp.data = "enabled";
        } else if (cmd == "off") {
            g_detector_enabled = false;
            LOG_INFO("检测器已禁用");
            resp.success = true;
            resp.data = "disabled";
        } else if (cmd == "status") {
            resp.success = true;
            resp.data = g_detector_enabled.load() ? "enabled" : "disabled";
        } else {
            resp.success = false;
            resp.data = "invalid_command: use 'on', 'off', or 'status'";
        }
        LOG_DEBUG("[onoff] 返回结果: success=%d, data=%s", resp.success, resp.data.c_str());
        return resp;
    });

    // 注册 set_threshold 服务处理函数：动态设置匹配阈值
    service->serve("set_threshold", [](const ServiceRequest& req) -> ServiceResponse {
        LOG_DEBUG("[set_threshold] 收到请求: %s", req.payload.c_str());
        ServiceResponse resp;
        
        try {
            float new_threshold = std::stof(req.payload);
            if (new_threshold < 0.0f || new_threshold > 1.0f) {
                resp.success = false;
                resp.data = "invalid_threshold: must be between 0.0 and 1.0";
                LOG_WARN("[set_threshold] 无效阈值: %f", new_threshold);
                return resp;
            }
            
            std::lock_guard<std::mutex> lock(g_detector_mutex);
            if (g_detector) {
                auto* cv_det = dynamic_cast<OpenCvTemplateDetector*>(g_detector.get());
                if (cv_det) {
                    cv_det->setMatchThreshold(new_threshold);
                    g_det_cfg.match_threshold = new_threshold;
                    resp.success = true;
                    resp.data = "threshold_set=" + std::to_string(new_threshold);
                    LOG_INFO("[set_threshold] 匹配阈值已更新为: %f", new_threshold);
                } else {
                    resp.success = false;
                    resp.data = "detector_type_not_supported";
                    LOG_WARN("[set_threshold] 当前检测器类型不支持动态设置阈值");
                }
            } else {
                resp.success = false;
                resp.data = "detector_not_ready";
                LOG_WARN("[set_threshold] 检测器未初始化");
            }
        } catch (const std::exception& e) {
            resp.success = false;
            resp.data = "invalid_format: " + std::string(e.what());
            LOG_ERROR("[set_threshold] 参数解析失败: %s", e.what());
        }
        
        return resp;
    });

    // 注册 set_v_threshold 服务处理函数：动态设置 V 通道阈值
    service->serve("set_v_threshold", [](const ServiceRequest& req) -> ServiceResponse {
        LOG_DEBUG("[set_v_threshold] 收到请求: %s", req.payload.c_str());
        ServiceResponse resp;
        
        try {
            int new_v_threshold = std::stoi(req.payload);
            if (new_v_threshold < 0 || new_v_threshold > 255) {
                resp.success = false;
                resp.data = "invalid_v_threshold: must be between 0 and 255";
                LOG_WARN("[set_v_threshold] 无效阈值: %d", new_v_threshold);
                return resp;
            }
            
            std::lock_guard<std::mutex> lock(g_detector_mutex);
            if (g_detector) {
                auto* cv_det = dynamic_cast<OpenCvTemplateDetector*>(g_detector.get());
                if (cv_det) {
                    cv_det->setVThreshold(new_v_threshold);
                    g_det_cfg.v_threshold = new_v_threshold;
                    resp.success = true;
                    resp.data = "v_threshold_set=" + std::to_string(new_v_threshold);
                    LOG_INFO("[set_v_threshold] V 通道阈值已更新为: %d", new_v_threshold);
                } else {
                    resp.success = false;
                    resp.data = "detector_type_not_supported";
                    LOG_WARN("[set_v_threshold] 当前检测器类型不支持动态设置 V 通道阈值");
                }
            } else {
                resp.success = false;
                resp.data = "detector_not_ready";
                LOG_WARN("[set_v_threshold] 检测器未初始化");
            }
        } catch (const std::exception& e) {
            resp.success = false;
            resp.data = "invalid_format: " + std::string(e.what());
            LOG_ERROR("[set_v_threshold] 参数解析失败: %s", e.what());
        }
        
        return resp;
    });

    // 注册 set_grad_threshold 服务处理函数：动态设置梯度阈值
    service->serve("set_grad_threshold", [](const ServiceRequest& req) -> ServiceResponse {
        LOG_DEBUG("[set_grad_threshold] 收到请求: %s", req.payload.c_str());
        ServiceResponse resp;
        
        try {
            int new_grad_threshold = std::stoi(req.payload);
            if (new_grad_threshold < 0 || new_grad_threshold > 255) {
                resp.success = false;
                resp.data = "invalid_grad_threshold: must be between 0 and 255";
                LOG_WARN("[set_grad_threshold] 无效阈值: %d", new_grad_threshold);
                return resp;
            }
            
            std::lock_guard<std::mutex> lock(g_detector_mutex);
            if (g_detector) {
                auto* cv_det = dynamic_cast<OpenCvTemplateDetector*>(g_detector.get());
                if (cv_det) {
                    cv_det->setGradientThreshold(new_grad_threshold);
                    g_det_cfg.grad_threshold = new_grad_threshold;
                    resp.success = true;
                    resp.data = "grad_threshold_set=" + std::to_string(new_grad_threshold);
                    LOG_INFO("[set_grad_threshold] 梯度阈值已更新为: %d", new_grad_threshold);
                } else {
                    resp.success = false;
                    resp.data = "detector_type_not_supported";
                    LOG_WARN("[set_grad_threshold] 当前检测器类型不支持动态设置梯度阈值");
                }
            } else {
                resp.success = false;
                resp.data = "detector_not_ready";
                LOG_WARN("[set_grad_threshold] 检测器未初始化");
            }
        } catch (const std::exception& e) {
            resp.success = false;
            resp.data = "invalid_format: " + std::string(e.what());
            LOG_ERROR("[set_grad_threshold] 参数解析失败: %s", e.what());
        }
        
        return resp;
    });

    // 注册 set_segment_mode 服务处理函数: 动态设置分割模式
    service->serve("set_segment_mode", [](const ServiceRequest& req) -> ServiceResponse {
        LOG_DEBUG("[set_segment_mode] 收到请求: %s", req.payload.c_str());
        ServiceResponse resp;
        std::string mode = req.payload;

        std::lock_guard<std::mutex> lock(g_detector_mutex);
        if (g_detector) {
            auto* cv_det = dynamic_cast<OpenCvTemplateDetector*>(g_detector.get());
            if (cv_det) {
                cv_det->setSegmentMode(mode);
                g_det_cfg.segment_mode = mode;
                resp.success = true;
                resp.data = "segment_mode_set=" + mode;
            } else {
                auto* eg_det = dynamic_cast<EdgeGradientDetector*>(g_detector.get());
                if (eg_det) {
                    eg_det->setSegmentMode(mode);
                    g_det_cfg.segment_mode = mode;
                    resp.success = true;
                    resp.data = "segment_mode_set=" + mode;
                } else {
                    resp.success = false;
                    resp.data = "detector_type_not_supported";
                }
            }
        } else {
            resp.success = false;
            resp.data = "detector_not_ready";
        }
        return resp;
    });

    // 注册 reload_template 服务处理函数：重新加载模版，不重启节点
    service->serve("reload_template", [](const ServiceRequest& req) -> ServiceResponse {
        (void)req;
        ServiceResponse resp;
        LOG_INFO("收到 reload_template 请求，重新初始化检测器...");
        auto new_det = createDetector(g_det_cfg);
        if (!new_det) {
            resp.success = false;
            resp.data = "创建检测器失败";
            LOG_ERROR("reload_template: 创建检测器失败");
            return resp;
        }
        if (!new_det->init()) {
            resp.success = false;
            resp.data = "检测器初始化失败（模版文件可能缺失）";
            LOG_ERROR("reload_template: 检测器初始化失败");
            return resp;
        }
        {
            std::lock_guard<std::mutex> lock(g_detector_mutex);
            g_detector = std::move(new_det);
        }
        g_detector_ready = true;
        LOG_INFO("reload_template: 检测器重新初始化完成");
        resp.success = true;
        resp.data = "reload_ok";
        return resp;
    });

    // 订阅帧回调: 直接处理，处理过程中到来的帧会被丢弃
    frame_sub->subscribe([](const FrameMsg& frame_msg) {
        // 检测器禁用时跳过
        if (!g_detector_enabled.load()) {
            return;
        }
        
        // 检测器未就绪时跳过
        if (!g_detector_ready.load()) {
            return;
        }

        // 尝试获取处理锁，如果正在处理则丢弃此帧
        bool expected = false;
        if (!g_processing_frame.compare_exchange_strong(expected, true)) {
            // 已有帧在处理中，丢弃当前帧
            static int drop_count = 0;
            if (++drop_count % 10 == 0) {
                LOG_WARN("检测器忙，已丢弃 %d 帧", drop_count);
            }
            return;
        }

        // 转换为 Frame 结构
        Frame frame = frameMsgToFrame(frame_msg);

        // 获取检测器指针（带锁）
        Detector* detector = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_detector_mutex);
            if (g_detector) {
                detector = g_detector.get();
            }
        }

        ObjectInfoList result;
        if (detector) {
            // 执行检测
            result = detector->detect(frame);
        }

        // 构建 DetectionMsg
        DetectionMsg det_msg;
        det_msg.protocol_string = result.toProtocolString();
        det_msg.frame_num = frame.frameNum;
        det_msg.timestamp = static_cast<int64_t>(getTimestampMs());
        det_msg.object_count = static_cast<int32_t>(result.size());

        // 构建 AnnotationMsg (用于分布式绘制)
        AnnotationMsg ann_msg;
        ann_msg.frame_num = frame.frameNum;
        ann_msg.timestamp = static_cast<int64_t>(getTimestampMs());
        
        // 获取模板尺寸（仅对基于模板的检测器有效）
        int tmpl_w = 0, tmpl_h = 0;
        if (detector) {
            if (auto* cv_det = dynamic_cast<OpenCvTemplateDetector*>(detector)) {
                tmpl_w = cv_det->getTemplateWidth();
                tmpl_h = cv_det->getTemplateHeight();
            } else if (auto* eg_det = dynamic_cast<EdgeGradientDetector*>(detector)) {
                tmpl_w = eg_det->getTemplateWidth();
                tmpl_h = eg_det->getTemplateHeight();
            } else if (auto* conv_det = dynamic_cast<ConveyorDetector*>(detector)) {
                tmpl_w = conv_det->getTemplateWidth();
                tmpl_h = conv_det->getTemplateHeight();
            }
        }
        ann_msg.template_width = static_cast<uint32_t>(tmpl_w);
        ann_msg.template_height = static_cast<uint32_t>(tmpl_h);
        
        // 填充物体标注列表
        int obj_id = 0;
        for (const auto& obj : result.getObjects()) {
            AnnotationMsg::ObjectAnnotation ann_obj;
            ann_obj.x = obj.getX();
            ann_obj.y = obj.getY();
            ann_obj.angle = obj.getAngle();
            ann_obj.type = obj.getType();
            ann_obj.id = obj_id++;
            ann_obj.score = 1.0;
            ann_msg.objects.push_back(ann_obj);
        }

        // 保存最新检测结果 (供服务端点查询)
        {
            std::lock_guard<std::mutex> lock(g_detection_mutex);
            g_latest_detection = det_msg;
        }

        // 发布检测结果（带锁保护）
        {
            std::lock_guard<std::mutex> lock(g_publisher_mutex);
            if (g_detection_pub) {
                g_detection_pub->publish(det_msg);
            }
            if (g_annotation_pub) {
                g_annotation_pub->publish(ann_msg);
                LOG_DEBUG("发布 AnnotationMsg: 帧#%d, 物体数=%d, 模板尺寸=%dx%d", 
                          ann_msg.frame_num, ann_msg.objects.size(), 
                          ann_msg.template_width, ann_msg.template_height);
            }
        }

        LOG_DEBUG("检测 帧#%d (%dx%d) 检测到 %d 个物体 -> %s", 
                  frame.frameNum, frame.width, frame.height, result.size(), 
                  det_msg.protocol_string.c_str());

        // 释放处理锁
        g_processing_frame = false;
    });

    // 所有服务注册完成，启动 ROS2 executor
    // ros2_global::start_executor() 已在 NodeFactory 构造时调用，无需重复
    // (原 ros2_global::start_executor() 调用已移除)

    LOG_INFO("Detector Node 运行中...");

    // 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("正在关闭检测器...");

    // 等待当前帧处理完成
    while (g_processing_frame.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 重置发布者（防止后续访问）
    {
        std::lock_guard<std::mutex> lock(g_publisher_mutex);
        g_detection_pub.reset();
        g_annotation_pub.reset();
    }

    // 重置检测器
    {
        std::lock_guard<std::mutex> lock(g_detector_mutex);
        g_detector.reset();
    }

    LOG_INFO("Detector Node 已退出");
    return 0;
}
