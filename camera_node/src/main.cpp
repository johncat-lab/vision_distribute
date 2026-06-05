#include "rpc/node_factory.h"
#include "rpc/config_loader.h"
#include "rpc/message_types.h"
#include "hik_camera.h"

#include <opencv2/core.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <mutex>
#include <sstream>

// ========== 全局运行标志 ==========
static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

// ========== 相机配置结构体 ==========
struct CameraConfig {
    int camera_index = 0;
    std::string trigger_mode = "off";
    std::string pixel_format = "Mono8";
    std::string exposure_auto = "continuous";
    float exposure_time = 10000.0f;
    std::string gain_auto = "continuous";
    float gain = 0.0f;
    float frame_rate = 30.0f;
};

// ========== 从 camera_config.xml 加载配置 (OpenCV FileStorage) ==========
static CameraConfig loadCameraConfig(const std::string& path) {
    CameraConfig cfg;
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        std::cerr << "[CameraNode] 无法打开相机配置文件: " << path
                  << "，使用默认配置" << std::endl;
        return cfg;
    }

    fs["camera_index"]  >> cfg.camera_index;
    fs["trigger_mode"]  >> cfg.trigger_mode;
    fs["pixel_format"]  >> cfg.pixel_format;
    fs["exposure_auto"] >> cfg.exposure_auto;
    fs["exposure_time"] >> cfg.exposure_time;
    fs["gain_auto"]     >> cfg.gain_auto;
    fs["gain"]          >> cfg.gain;
    fs["frame_rate"]    >> cfg.frame_rate;

    fs.release();
    return cfg;
}

// ========== 枚举转换辅助函数 ==========
static void parseTriggerMode(const std::string& mode_str,
                             TriggerMode& mode,
                             TriggerSource& source) {
    if (mode_str == "off") {
        mode = TriggerMode::OFF;
        source = TriggerSource::LINE0;
    } else {
        mode = TriggerMode::ON;
        if (mode_str == "line0")          source = TriggerSource::LINE0;
        else if (mode_str == "line1")     source = TriggerSource::LINE1;
        else if (mode_str == "line2")     source = TriggerSource::LINE2;
        else if (mode_str == "software")  source = TriggerSource::SOFTWARE;
        else                              source = TriggerSource::LINE0;
    }
}

static ExposureAuto parseExposureAuto(const std::string& str) {
    if (str == "off")        return ExposureAuto::OFF;
    if (str == "once")       return ExposureAuto::ONCE;
    return ExposureAuto::CONTINUOUS;
}

static GainAuto parseGainAuto(const std::string& str) {
    if (str == "off")   return GainAuto::OFF;
    if (str == "once")  return GainAuto::ONCE;
    return GainAuto::CONTINUOUS;
}

// ========== 用法说明 ==========
static void printUsage(const char* prog) {
    std::cout << "用法: " << prog
              << " --config <system_config.xml>"
              << " --camera-config <camera_config.xml>"
              << std::endl;
}

// ========== 主函数 ==========
int main(int argc, char* argv[]) {
    std::string config_path;
    std::string camera_config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--camera-config" && i + 1 < argc) {
            camera_config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (config_path.empty()) {
        std::cerr << "错误: 未指定系统配置文件" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    // 注册信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ---- 1. 加载系统配置 ----
    NodeConfig config;
    try {
        config = ConfigLoader::loadSystemConfig(config_path);
        config.node_name = "camera_node";
    } catch (const std::exception& e) {
        std::cerr << "加载系统配置失败: " << e.what() << std::endl;
        return 1;
    }

    std::string transport_name;
    switch (config.transport) {
    case TransportType::ZEROMQ: transport_name = "ZeroMQ"; break;
    case TransportType::ZENOH:  transport_name = "Zenoh"; break;
    case TransportType::ROS2:   transport_name = "ROS2"; break;
    }
    std::cout << "[CameraNode] 传输方式: " << transport_name << std::endl;

    // ---- 2. 加载相机配置 ----
    CameraConfig cam_cfg;
    if (!camera_config_path.empty()) {
        cam_cfg = loadCameraConfig(camera_config_path);
        std::cout << "[CameraNode] 相机配置已加载: " << camera_config_path << std::endl;
    } else {
        std::cout << "[CameraNode] 未指定相机配置，使用默认值" << std::endl;
    }

    // ---- 3. 创建 NodeFactory ----
    NodeFactory factory(config);

    // ---- 4. 创建帧发布者 (topic: vision/frame) ----
    auto frame_pub = factory.createPublisher<FrameMsg>("vision/frame");
    std::cout << "[CameraNode] 帧发布者已创建，topic: vision/frame" << std::endl;

    // ---- 5. 创建相机配置服务 ----
    auto camera_service = factory.createService<ServiceRequest, ServiceResponse>("camera");
    std::cout << "[CameraNode] 相机服务已创建" << std::endl;

    // ---- 6. 初始化 HikCamera ----
    HikCamera camera;

    std::vector<CameraDeviceInfo> devices;
    if (!camera.enumDevices(devices) || devices.empty()) {
        std::cerr << "[CameraNode] 未发现相机设备" << std::endl;
        return 1;
    }
    std::cout << "[CameraNode] 发现 " << devices.size() << " 个相机设备" << std::endl;
    for (const auto& dev : devices) {
        std::cout << "  [" << dev.index << "] " << dev.model_name
                  << " (" << dev.serial_number << ") " << dev.transport_type << std::endl;
    }

    if (!camera.open(cam_cfg.camera_index)) {
        std::cerr << "[CameraNode] 无法打开相机 (index=" << cam_cfg.camera_index << ")" << std::endl;
        return 1;
    }

    // ---- 7. 应用相机配置 ----
    camera.setPixelFormat(cam_cfg.pixel_format);

    TriggerMode trig_mode;
    TriggerSource trig_source;
    parseTriggerMode(cam_cfg.trigger_mode, trig_mode, trig_source);
    camera.setTriggerMode(trig_mode);
    if (trig_mode == TriggerMode::ON) {
        camera.setTriggerSource(trig_source);
    }

    camera.setExposureAuto(parseExposureAuto(cam_cfg.exposure_auto));
    camera.setExposureTime(cam_cfg.exposure_time);
    camera.setGainAuto(parseGainAuto(cam_cfg.gain_auto));
    camera.setGain(cam_cfg.gain);
    camera.setFrameRate(cam_cfg.frame_rate);

    std::cout << "[CameraNode] 相机参数已配置" << std::endl;

    // ---- 8. 设置图像回调 ----
    // 使用 mutex 保护相机配置的并发访问
    auto cam_mutex = std::make_shared<std::mutex>();

    camera.setImageCallback([&frame_pub, &cam_cfg](const FrameInfo& info) {
        FrameMsg msg;
        msg.camera_id = cam_cfg.camera_index;
        msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        msg.width = static_cast<uint16_t>(info.width);
        msg.height = static_cast<uint16_t>(info.height);
        msg.pixel_type = static_cast<uint32_t>(info.pixelType);
        msg.frame_num = info.frameNum;
        msg.exposure_time = info.exposureTime;
        msg.gain = info.gain;
        // 复制图像数据（回调返回后原始指针失效）
        if (info.data && info.dataLen > 0) {
            msg.data.assign(info.data, info.data + info.dataLen);
        }

        frame_pub->publish(msg);
    });

    // ---- 9. 注册服务端点 ----
    // camera/set_exposure
    camera_service->serve("camera/set_exposure",
        [&camera, cam_mutex, &cam_cfg](const ServiceRequest& req) -> ServiceResponse {
            std::lock_guard<std::mutex> lock(*cam_mutex);
            ServiceResponse resp;
            try {
                float exposure = std::stof(req.payload);
                if (camera.setExposureTime(exposure)) {
                    cam_cfg.exposure_time = exposure;
                    resp.success = true;
                    resp.data = "曝光时间已设置: " + std::to_string(static_cast<int>(exposure)) + " us";
                } else {
                    resp.success = false;
                    resp.data = "设置曝光时间失败";
                }
            } catch (const std::exception& e) {
                resp.success = false;
                resp.data = std::string("参数错误: ") + e.what();
            }
            return resp;
        });

    // camera/set_gain
    camera_service->serve("camera/set_gain",
        [&camera, cam_mutex, &cam_cfg](const ServiceRequest& req) -> ServiceResponse {
            std::lock_guard<std::mutex> lock(*cam_mutex);
            ServiceResponse resp;
            try {
                float gain = std::stof(req.payload);
                if (camera.setGain(gain)) {
                    cam_cfg.gain = gain;
                    resp.success = true;
                    resp.data = "增益已设置: " + std::to_string(gain);
                } else {
                    resp.success = false;
                    resp.data = "设置增益失败";
                }
            } catch (const std::exception& e) {
                resp.success = false;
                resp.data = std::string("参数错误: ") + e.what();
            }
            return resp;
        });

    // camera/set_trigger_mode
    camera_service->serve("camera/set_trigger_mode",
        [&camera, cam_mutex, &cam_cfg](const ServiceRequest& req) -> ServiceResponse {
            std::lock_guard<std::mutex> lock(*cam_mutex);
            ServiceResponse resp;
            TriggerMode mode;
            TriggerSource source;
            parseTriggerMode(req.payload, mode, source);
            bool ok = camera.setTriggerMode(mode);
            if (ok && mode == TriggerMode::ON) {
                ok = camera.setTriggerSource(source);
            }
            if (ok) {
                cam_cfg.trigger_mode = req.payload;
                resp.success = true;
                resp.data = "触发模式已设置: " + req.payload;
            } else {
                resp.success = false;
                resp.data = "设置触发模式失败";
            }
            return resp;
        });

    // camera/soft_trigger
    camera_service->serve("camera/soft_trigger",
        [&camera](const ServiceRequest&) -> ServiceResponse {
            ServiceResponse resp;
            if (camera.triggerSoftware()) {
                resp.success = true;
                resp.data = "软触发已执行";
            } else {
                resp.success = false;
                resp.data = "软触发执行失败";
            }
            return resp;
        });

    // camera/get_config
    camera_service->serve("camera/get_config",
        [&cam_cfg](const ServiceRequest&) -> ServiceResponse {
            ServiceResponse resp;
            resp.success = true;
            std::ostringstream oss;
            oss << "camera_index=" << cam_cfg.camera_index
                << "\ntrigger_mode=" << cam_cfg.trigger_mode
                << "\npixel_format=" << cam_cfg.pixel_format
                << "\nexposure_auto=" << cam_cfg.exposure_auto
                << "\nexposure_time=" << cam_cfg.exposure_time
                << "\ngain_auto=" << cam_cfg.gain_auto
                << "\ngain=" << cam_cfg.gain
                << "\nframe_rate=" << cam_cfg.frame_rate;
            resp.data = oss.str();
            return resp;
        });

    std::cout << "[CameraNode] 服务端点已注册:" << std::endl;
    std::cout << "  camera/set_exposure" << std::endl;
    std::cout << "  camera/set_gain" << std::endl;
    std::cout << "  camera/set_trigger_mode" << std::endl;
    std::cout << "  camera/soft_trigger" << std::endl;
    std::cout << "  camera/get_config" << std::endl;

    // ---- 10. 开始取流 ----
    if (!camera.startGrabbing()) {
        std::cerr << "[CameraNode] 无法开始取流" << std::endl;
        camera.close();
        return 1;
    }
    std::cout << "[CameraNode] 相机取流已启动" << std::endl;

    // ---- 11. 主循环 ----
    std::cout << "[CameraNode] 运行中..." << std::endl;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ---- 清理 ----
    camera.stopGrabbing();
    camera.close();
    std::cout << "[CameraNode] 已停止" << std::endl;

    return 0;
}
