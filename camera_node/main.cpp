#include "rpc/node_factory.h"
#include "rpc/config_loader.h"
#include "rpc/message_types.h"
#include "hik_camera.h"
#include "logger/logger.h"

// 类型别名：兼容 vision_client hik_camera.h 的前缀命名
using TriggerMode = HikTriggerMode;
using TriggerSource = HikTriggerSource;
using ExposureAuto = HikExposureAuto;
using GainAuto = HikGainAuto;
using FrameInfo = HikFrameInfo;

// CameraDeviceInfo：封装 MV_CC_DEVICE_INFO 为 main.cpp 使用的简化结构
struct CameraDeviceInfo {
    int index = 0;
    std::string model_name;
    std::string serial_number;
    std::string transport_type;
};

#include <opencv2/core.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <mutex>
#include <sstream>

#ifdef HAS_ROS2
#include "ros2_backend.h"
#include "vision_interfaces/srv/camera_get_config.hpp"
#include "vision_interfaces/srv/camera_set_exposure.hpp"
#include "vision_interfaces/srv/camera_set_gain.hpp"
#include "vision_interfaces/srv/camera_set_trigger_mode.hpp"
#include "vision_interfaces/srv/camera_soft_trigger.hpp"
#endif

// ========== 全局运行标志 ==========
static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

// ========== 相机配置结构体 ==========
struct CameraConfig {
    int camera_index = 0;
    std::string trigger_mode = "continuous";
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
        LOG_WARN("[CameraNode] 无法打开相机配置文件: %s，使用默认配置", path.c_str());
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
    if (mode_str == "off" || mode_str == "continuous") {
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

static void printUsage(const char* prog) {
    LOG_INFO("用法: %s --config <system_config.xml> --camera-config <camera_config.xml>", prog);
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
        LOG_ERROR("未指定系统配置文件");
        printUsage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    NodeConfig config;
    try {
        config = ConfigLoader::loadSystemConfig(config_path);
        config.node_name = "camera_node";
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
    LOG_INFO("[CameraNode] 传输方式: %s", transport_name.c_str());

    CameraConfig cam_cfg;
    if (!camera_config_path.empty()) {
        cam_cfg = loadCameraConfig(camera_config_path);
        LOG_INFO("[CameraNode] 相机配置已加载: %s", camera_config_path.c_str());
    } else {
        LOG_INFO("[CameraNode] 未指定相机配置，使用默认值");
    }

    // ---- 3. 创建 NodeFactory ----
    NodeFactory factory(config);

    auto frame_pub = factory.createPublisher<FrameMsg>("vision/frame");
    LOG_INFO("[CameraNode] 帧发布者已创建，topic: vision/frame");

    auto camera_service = factory.createService<ServiceRequest, ServiceResponse>("camera");
    LOG_INFO("[CameraNode] 相机服务已创建");

    // ---- 6. 初始化 HikCamera ----
    HikCamera camera;
    std::atomic<bool> camera_ready{false};

    std::vector<MV_CC_DEVICE_INFO> raw_devices;
    if (!camera.enumDevices(raw_devices) || raw_devices.empty()) {
        LOG_WARN("[CameraNode] 未发现相机设备，将以未就绪状态运行");
    } else {
        std::vector<CameraDeviceInfo> devices;
        for (size_t i = 0; i < raw_devices.size(); ++i) {
            CameraDeviceInfo dev;
            dev.index = static_cast<int>(i);
            dev.model_name = "HikCamera";
            dev.serial_number = std::to_string(i);
            dev.transport_type = "GigE/USB";
            devices.push_back(dev);
        }
        LOG_INFO("[CameraNode] 发现 %d 个相机设备", devices.size());
        for (const auto& dev : devices) {
            LOG_INFO("[CameraNode]   [%d] %s (%s) %s", dev.index, dev.model_name.c_str(), dev.serial_number.c_str(), dev.transport_type.c_str());
        }

        if (!camera.open(cam_cfg.camera_index)) {
            LOG_ERROR("[CameraNode] 无法打开相机 (index=%d)，将以未就绪状态运行", cam_cfg.camera_index);
        } else {
            // ---- 7. 应用相机配置 ----
            camera.setPixelFormat(cam_cfg.pixel_format);

            TriggerMode trig_mode;
            TriggerSource trig_source;
            parseTriggerMode(cam_cfg.trigger_mode, trig_mode, trig_source);
            camera.setTriggerMode(trig_mode);
            if (trig_mode == TriggerMode::ON) {
                camera.setTriggerSource(trig_source);
            }

            auto expAuto = parseExposureAuto(cam_cfg.exposure_auto);
            camera.setExposureAuto(expAuto);
            if (expAuto == ExposureAuto::OFF) {
                camera.setExposureTime(cam_cfg.exposure_time);
            } else {
                LOG_INFO("[CameraNode] 自动曝光模式开启，跳过手动曝光时间设置");
            }

            auto gainAuto = parseGainAuto(cam_cfg.gain_auto);
            camera.setGainAuto(gainAuto);
            if (gainAuto == GainAuto::OFF) {
                camera.setGain(cam_cfg.gain);
            } else {
                LOG_INFO("[CameraNode] 自动增益模式开启，跳过手动增益设置");
            }
            camera.setFrameRate(cam_cfg.frame_rate);

            LOG_INFO("[CameraNode] 相机参数已配置");

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

            camera_ready = true;
        }
    }

    // ---- 7. 注册服务端点 ----
    // 使用全局 mutex 保护相机配置的并发访问
    auto cam_mutex = std::make_shared<std::mutex>();

#ifdef HAS_ROS2
    // 注册原生 ROS2 service 类型映射（双向转换：服务端+客户端）
    // serve() 会自动创建对应的原生 service，call() 也通过原生 client 调用
    if (auto* rs = dynamic_cast<Ros2Service<ServiceRequest, ServiceResponse>*>(camera_service.get())) {
        rs->registerNativeEndpoint<vision_interfaces::srv::CameraSetExposure>(
            "set_exposure",
            // 服务端: 原生请求 → ServiceRequest
            [](auto req) -> ServiceRequest {
                ServiceRequest sr;
                sr.payload = std::to_string(req->exposure_time);
                return sr;
            },
            // 服务端: ServiceResponse → 原生响应
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            // 客户端: ServiceRequest → 原生请求
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CameraSetExposure::Request> {
                auto req = std::make_shared<vision_interfaces::srv::CameraSetExposure::Request>();
                req->exposure_time = std::stof(sr.payload);
                return req;
            },
            // 客户端: 原生响应 → ServiceResponse
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CameraSetGain>(
            "set_gain",
            [](auto req) -> ServiceRequest {
                ServiceRequest sr;
                sr.payload = std::to_string(req->gain);
                return sr;
            },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CameraSetGain::Request> {
                auto req = std::make_shared<vision_interfaces::srv::CameraSetGain::Request>();
                req->gain = std::stof(sr.payload);
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CameraSetTriggerMode>(
            "set_trigger_mode",
            [](auto req) -> ServiceRequest {
                ServiceRequest sr;
                sr.payload = req->mode;
                return sr;
            },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CameraSetTriggerMode::Request> {
                auto req = std::make_shared<vision_interfaces::srv::CameraSetTriggerMode::Request>();
                req->mode = sr.payload;
                return req;
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CameraSoftTrigger>(
            "soft_trigger",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CameraSoftTrigger::Request> {
                return std::make_shared<vision_interfaces::srv::CameraSoftTrigger::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CameraGetConfig>(
            "get_config",
            [](auto) -> ServiceRequest { return ServiceRequest{}; },
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->config_data = sr.data;
            },
            [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CameraGetConfig::Request> {
                return std::make_shared<vision_interfaces::srv::CameraGetConfig::Request>();
            },
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->config_data;
                return sr;
            });
    }
#endif

    // set_exposure
    camera_service->serve("set_exposure",
        [&camera, &camera_ready, cam_mutex, &cam_cfg](const ServiceRequest& req) -> ServiceResponse {
            ServiceResponse resp;
            if (!camera_ready) {
                resp.success = false;
                resp.data = "相机未就绪";
                return resp;
            }
            std::lock_guard<std::mutex> lock(*cam_mutex);
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

    // set_gain
    camera_service->serve("set_gain",
        [&camera, &camera_ready, cam_mutex, &cam_cfg](const ServiceRequest& req) -> ServiceResponse {
            ServiceResponse resp;
            if (!camera_ready) {
                resp.success = false;
                resp.data = "相机未就绪";
                return resp;
            }
            std::lock_guard<std::mutex> lock(*cam_mutex);
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

    // set_trigger_mode
    camera_service->serve("set_trigger_mode",
        [&camera, &camera_ready, cam_mutex, &cam_cfg](const ServiceRequest& req) -> ServiceResponse {
            ServiceResponse resp;
            if (!camera_ready) {
                resp.success = false;
                resp.data = "相机未就绪";
                return resp;
            }
            std::lock_guard<std::mutex> lock(*cam_mutex);
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

    // soft_trigger
    camera_service->serve("soft_trigger",
        [&camera, &camera_ready](const ServiceRequest&) -> ServiceResponse {
            ServiceResponse resp;
            if (!camera_ready) {
                resp.success = false;
                resp.data = "相机未就绪";
                return resp;
            }
            if (camera.triggerSoftware()) {
                resp.success = true;
                resp.data = "软触发已执行";
            } else {
                resp.success = false;
                resp.data = "软触发执行失败";
            }
            return resp;
        });

    // get_config
    camera_service->serve("get_config",
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

    LOG_INFO("[CameraNode] 服务端点已注册:");
    LOG_INFO("  camera/set_exposure");
    LOG_INFO("  camera/set_gain");
    LOG_INFO("  camera/set_trigger_mode");
    LOG_INFO("  camera/soft_trigger");
    LOG_INFO("  camera/get_config");

    bool streaming = false;
    if (camera_ready) {
        if (!camera.startGrabbing()) {
            LOG_ERROR("[CameraNode] 无法开始取流，将以无流模式运行");
        } else {
            LOG_INFO("[CameraNode] 相机取流已启动");
            streaming = true;
        }
    } else {
        LOG_INFO("[CameraNode] 相机未就绪，跳过取流");
    }

    LOG_INFO("[CameraNode] 运行中...");
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (streaming) {
        camera.stopGrabbing();
    }
    camera.close();
    LOG_INFO("[CameraNode] 已停止");

    return 0;
}
