#include "rpc/node_factory.h"
#include "rpc/config_loader.h"
#include "rpc/message_types.h"

#include "detector.h"
#include "opencv_template_detector.h"
#include "edge_gradient_detector.h"
#include "conveyor_detector.h"
#include "yolo_detector.h"
#include "object_info.h"
#include "frame_queue.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <memory>
#include <csignal>
#include <sstream>
#include <opencv2/core.hpp>

// ========== 全局状态 ==========
static std::unique_ptr<Detector> g_detector;
static DetectionMsg g_latest_detection;
static std::mutex g_detection_mutex;
static std::string g_detector_config_summary;
static std::atomic<bool> g_running{true};

// 全局发布者 (需要在回调中使用)
static std::shared_ptr<IPublisher<DetectionMsg>> g_detection_pub;

// ========== 信号处理 ==========
static void signalHandler(int sig) {
    (void)sig;
    std::cout << "\n[信息] 收到退出信号，正在关闭..." << std::endl;
    g_running = false;
}

// ========== 命令行用法 ==========
static void printUsage(const char* prog) {
    std::cout << "用法: " << prog << " --config <system_config.xml> --detector-config <detector.xml>" << std::endl;
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
};

// ========== 加载检测器配置 ==========
static DetectorConfig loadDetectorConfig(const std::string& config_path) {
    DetectorConfig cfg;

    cv::FileStorage fs(config_path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        std::cerr << "[错误] 无法打开检测器配置文件: " << config_path << std::endl;
        return cfg;
    }

    if (!fs["detector"].empty())         cfg.detector = (std::string)fs["detector"];
    if (!fs["template_dir"].empty())     cfg.template_dir = (std::string)fs["template_dir"];
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
        if (cfg.roi_y_center >= 0) cv_det->setRoiYCenter(cfg.roi_y_center);
        if (cfg.roi_y_margin >= 0) cv_det->setRoiYMargin(cfg.roi_y_margin);
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
        if (!cfg.bg_ref_path.empty()) conv_det->setBgRefPath(cfg.bg_ref_path);
        conv_det->setVerifyWithTemplate(cfg.verify_with_template);
        return std::unique_ptr<Detector>(conv_det);
    } else if (cfg.detector == "yolo") {
        return std::make_unique<YoloDetector>(cfg.model_path, 0.5f, 0.45f);
    }

    std::cerr << "[错误] 未知的检测器类型: " << cfg.detector << std::endl;
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
        oss << ",model_path=" << cfg.model_path;
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
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--detector-config" && i + 1 < argc) {
            detector_config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (config_path.empty()) {
        std::cerr << "[错误] 未指定系统配置文件" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    if (detector_config_path.empty()) {
        std::cerr << "[错误] 未指定检测器配置文件 (--detector-config)" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    // 加载系统配置
    NodeConfig config;
    try {
        config = ConfigLoader::loadSystemConfig(config_path);
        config.node_name = "detector_node";
    } catch (const std::exception& e) {
        std::cerr << "[错误] 加载系统配置失败: " << e.what() << std::endl;
        return 1;
    }

    std::string transport_name;
    switch (config.transport) {
    case TransportType::ZEROMQ: transport_name = "ZeroMQ"; break;
    case TransportType::ZENOH:  transport_name = "Zenoh"; break;
    case TransportType::ROS2:   transport_name = "ROS2"; break;
    }

    std::cout << "[信息] Detector Node 启动" << std::endl;
    std::cout << "[信息] 传输方式: " << transport_name << std::endl;
    std::cout << "[信息] 检测器配置文件: " << detector_config_path << std::endl;

    // 加载检测器配置
    DetectorConfig det_cfg = loadDetectorConfig(detector_config_path);
    std::cout << "[信息] 检测器类型: " << det_cfg.detector << std::endl;
    std::cout << "[信息] 模版目录: " << det_cfg.template_dir << std::endl;
    std::cout << "[信息] 匹配阈值: " << det_cfg.match_threshold << std::endl;
    std::cout << "[信息] 分割模式: " << det_cfg.segment_mode << std::endl;

    g_detector_config_summary = makeConfigSummary(det_cfg);

    // 创建检测器实例
    g_detector = createDetector(det_cfg);
    if (!g_detector) {
        std::cerr << "[错误] 创建检测器失败" << std::endl;
        return 1;
    }

    if (!g_detector->init()) {
        std::cerr << "[错误] 检测器初始化失败" << std::endl;
        return 1;
    }

    std::cout << "[信息] 检测器初始化完成" << std::endl;

    // 注册信号处理
    signal(SIGINT, signalHandler);
#ifndef _WIN32
    signal(SIGTERM, signalHandler);
#endif

    // 创建 NodeFactory
    NodeFactory factory(config);

    // 创建帧订阅者 (vision/frame)
    auto frame_sub = factory.createSubscriber<FrameMsg>("vision/frame");

    // 创建检测发布者 (vision/detection)
    g_detection_pub = factory.createPublisher<DetectionMsg>("vision/detection");

    // 创建服务 (detector/get_result, detector/get_config)
    auto service = factory.createService<ServiceRequest, ServiceResponse>("detector");

    // 注册 get_result 服务处理函数
    service->serve("detector/get_result", [](const ServiceRequest& req) -> ServiceResponse {
        (void)req;
        ServiceResponse resp;
        std::lock_guard<std::mutex> lock(g_detection_mutex);
        resp.success = true;
        resp.data = g_latest_detection.serialize();
        return resp;
    });

    // 注册 get_config 服务处理函数
    service->serve("detector/get_config", [](const ServiceRequest& req) -> ServiceResponse {
        (void)req;
        ServiceResponse resp;
        resp.success = true;
        resp.data = g_detector_config_summary;
        return resp;
    });

    // 订阅帧回调: 反序列化 FrameMsg -> 转换为 Frame -> 检测 -> 发布 DetectionMsg
    frame_sub->subscribe([](const FrameMsg& frame_msg) {
        // 转换为 Frame 结构
        Frame frame = frameMsgToFrame(frame_msg);

        // 执行检测
        ObjectInfoList result = g_detector->detect(frame);

        // 构建 DetectionMsg
        DetectionMsg det_msg;
        det_msg.protocol_string = result.toProtocolString();
        det_msg.frame_num = frame.frameNum;
        det_msg.timestamp = static_cast<int64_t>(getTimestampMs());
        det_msg.object_count = static_cast<int32_t>(result.size());

        // 保存最新检测结果 (供服务端点查询)
        {
            std::lock_guard<std::mutex> lock(g_detection_mutex);
            g_latest_detection = det_msg;
        }

        // 发布检测结果
        g_detection_pub->publish(det_msg);

        std::cout << "[检测] 帧#" << frame.frameNum
                  << " (" << frame.width << "x" << frame.height << ")"
                  << " 检测到 " << result.size() << " 个物体"
                  << " -> " << det_msg.protocol_string << std::endl;
    });

    std::cout << "[信息] Detector Node 运行中..." << std::endl;

    // 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    g_detector.reset();
    std::cout << "[信息] Detector Node 已退出" << std::endl;
    return 0;
}
