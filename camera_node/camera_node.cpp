#include "camera_node.h"
#include "rpc/node_factory.h"
#include "rpc/edge_manager.h"
#include "rpc/service_endpoint_registry.h"
#include "logger/logger.h"

#ifdef HAS_ROS2
#include "vision_interfaces/srv/camera_set_exposure.hpp"
#include "vision_interfaces/srv/camera_set_gain.hpp"
#include "vision_interfaces/srv/camera_set_trigger_mode.hpp"
#include "vision_interfaces/srv/camera_soft_trigger.hpp"
#include "vision_interfaces/srv/camera_get_config.hpp"
#endif

// 类型别名（与原 main.cpp 保持一致）
using TriggerMode = HikTriggerMode;
using TriggerSource = HikTriggerSource;
using ExposureAuto = HikExposureAuto;
using GainAuto = HikGainAuto;
using FrameInfo = HikFrameInfo;

#include <opencv2/core.hpp>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

// ========== CameraNode 配置加载 ==========
CameraNode::CameraConfig CameraNode::loadCameraConfig(const std::string& path) {
    CameraConfig cfg;
    if (path.empty()) {
        LOG_INFO("[CameraNode] 未指定相机配置，使用默认值");
        return cfg;
    }
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        LOG_WARN("[CameraNode] 无法打开相机配置文件: %s，使用默认配置",
                 path.c_str());
        return cfg;
    }
    
    // 加载相机类型 (新增)
    fs["camera_type"] >> cfg.camera_type;
    
    fs["camera_index"]  >> cfg.camera_index;
    fs["trigger_mode"]  >> cfg.trigger_mode;
    fs["pixel_format"]  >> cfg.pixel_format;
    fs["exposure_auto"] >> cfg.exposure_auto;
    fs["exposure_time"] >> cfg.exposure_time;
    fs["gain_auto"]     >> cfg.gain_auto;
    fs["gain"]          >> cfg.gain;
    fs["frame_rate"]    >> cfg.frame_rate;
    fs.release();
    
    LOG_INFO("[CameraNode] 相机配置已加载: %s, 类型=%s", 
             path.c_str(), cfg.camera_type.c_str());
    return cfg;
}

// 枚举转换辅助
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
    if (str == "off")   return ExposureAuto::OFF;
    if (str == "once")  return ExposureAuto::ONCE;
    return ExposureAuto::CONTINUOUS;
}

static GainAuto parseGainAuto(const std::string& str) {
    if (str == "off")   return GainAuto::OFF;
    if (str == "once")  return GainAuto::ONCE;
    return GainAuto::CONTINUOUS;
}

// ========== CameraNode 析构函数 ==========
CameraNode::~CameraNode() {
    stop();
}

// ========== NodeManifest ==========
NodeManifest CameraNode::describe() const {
    NodeManifest m;
    m.name = "camera_node";
    m.binary = "camera_node";
    m.version = "1.0";
    m.config_file = "camera_config.xml";
    m.outputs.push_back({"frame_output", "FrameMsg", "相机采集的图像帧"});
    m.provides_services.push_back({"camera", {"set_exposure", "set_gain",
        "set_trigger_mode", "soft_trigger", "get_config"}});
    return m;
}

// ========== initDataflow：创建发布者 ==========
void CameraNode::initDataflow(NodeEdgeManager& edges,
                               const std::string& config_file) {
    edges.setDefaultTopic("frame_output", "vision/frame");
    frame_pub_ = edges.publish<FrameMsg>("frame_output", "vision/frame");
    LOG_INFO("[CameraNode] 帧发布者已创建，topic: %s",
             frame_pub_->getTopic().c_str());

    // 加载相机配置
    cam_cfg_ = loadCameraConfig(config_file);
}

// ========== initServices：注册服务端点 ==========
void CameraNode::initServices(ServiceEndpointRegistry& services) {
    services.registerEndpoint({"set_exposure", "设置曝光时间", false, 0},
        [this](const ServiceRequest& req) { return handleSetExposure(req); });

    services.registerEndpoint({"set_gain", "设置增益", false, 0},
        [this](const ServiceRequest& req) { return handleSetGain(req); });

    services.registerEndpoint({"set_trigger_mode", "设置触发模式", false, 0},
        [this](const ServiceRequest& req) { return handleSetTriggerMode(req); });

    services.registerEndpoint({"soft_trigger", "软触发一次", false, 0},
        [this](const ServiceRequest& req) { return handleSoftTrigger(req); });

    services.registerEndpoint({"get_config", "获取当前配置", false, 0},
        [this](const ServiceRequest& req) { return handleGetConfig(req); });

    LOG_INFO("[CameraNode] 已注册 5 个服务端点");
}

// ========== initServices (ROS2 原生类型注册) ==========
void CameraNode::initServices(ServiceEndpointRegistry& services, NodeContainer& container) {
    // 先注册通用端点处理函数
    initServices(services);

#ifdef HAS_ROS2
    // 注册 ROS2 原生 .srv 类型映射
    container.registerRos2NativeEndpoint<vision_interfaces::srv::CameraSetExposure>(
        "set_exposure",
        [](auto req) -> ServiceRequest {
            ServiceRequest sr;
            sr.set_payload(std::to_string(req->exposure_time));
            return sr;
        },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->message = sr.data();
        },
        [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CameraSetExposure::Request> {
            auto req = std::make_shared<vision_interfaces::srv::CameraSetExposure::Request>();
            req->exposure_time = std::stof(sr.payload());
            return req;
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->message);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::CameraSetGain>(
        "set_gain",
        [](auto req) -> ServiceRequest {
            ServiceRequest sr;
            sr.set_payload(std::to_string(req->gain));
            return sr;
        },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->message = sr.data();
        },
        [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CameraSetGain::Request> {
            auto req = std::make_shared<vision_interfaces::srv::CameraSetGain::Request>();
            req->gain = std::stof(sr.payload());
            return req;
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->message);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::CameraSetTriggerMode>(
        "set_trigger_mode",
        [](auto req) -> ServiceRequest {
            ServiceRequest sr;
            sr.set_payload(req->mode);
            return sr;
        },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->message = sr.data();
        },
        [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CameraSetTriggerMode::Request> {
            auto req = std::make_shared<vision_interfaces::srv::CameraSetTriggerMode::Request>();
            req->mode = sr.payload();
            return req;
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->message);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::CameraSoftTrigger>(
        "soft_trigger",
        [](auto) -> ServiceRequest { return ServiceRequest{}; },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->message = sr.data();
        },
        [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CameraSoftTrigger::Request> {
            return std::make_shared<vision_interfaces::srv::CameraSoftTrigger::Request>();
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->message);
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::CameraGetConfig>(
        "get_config",
        [](auto) -> ServiceRequest { return ServiceRequest{}; },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success();
            resp->config_data = sr.data();
        },
        [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CameraGetConfig::Request> {
            return std::make_shared<vision_interfaces::srv::CameraGetConfig::Request>();
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.set_success(resp->success);
            sr.set_data(resp->config_data);
            return sr;
        });

    LOG_INFO("[CameraNode] 已注册 5 个 ROS2 原生 service 类型映射");
#endif
}

// ========== start：打开相机并开始采集 ==========
bool CameraNode::start() {
    // 1. 根据配置创建相机实例
    camera_ = CameraFactory::create(cam_cfg_.camera_type);
    if (!camera_) {
        LOG_ERROR("[CameraNode] 无法创建相机实例: %s", cam_cfg_.camera_type.c_str());
        return false;
    }
    
    // 2. 枚举设备
    std::vector<CameraDeviceInfo> devices;
    if (!camera_->enumDevices(devices) || devices.empty()) {
        LOG_WARN("[CameraNode] 未发现相机设备，将以未就绪状态运行");
        camera_ready_ = false;
        return true;
    }
    
    LOG_INFO("[CameraNode] 发现 %zu 个相机设备", devices.size());
    
    // 3. 打开相机
    if (!camera_->open(cam_cfg_.camera_index)) {
        LOG_ERROR("[CameraNode] 无法打开相机 (type=%s, index=%d)", 
                  cam_cfg_.camera_type.c_str(), cam_cfg_.camera_index);
        return false;
    }
    
    // 4. 配置相机参数
    camera_->setPixelFormat(cam_cfg_.pixel_format);
    
    TriggerMode trig_mode;
    TriggerSource trig_source;
    parseTriggerMode(cam_cfg_.trigger_mode, trig_mode, trig_source);
    camera_->setTriggerMode(trig_mode);
    if (trig_mode == TriggerMode::ON) {
        camera_->setTriggerSource(trig_source);
    }
    
    auto expAuto = parseExposureAuto(cam_cfg_.exposure_auto);
    camera_->setExposureAuto(expAuto);
    if (expAuto == ExposureAuto::OFF) {
        camera_->setExposureTime(cam_cfg_.exposure_time);
    }
    
    auto gainAuto = parseGainAuto(cam_cfg_.gain_auto);
    camera_->setGainAuto(gainAuto);
    if (gainAuto == GainAuto::OFF) {
        camera_->setGain(cam_cfg_.gain);
    }
    camera_->setFrameRate(cam_cfg_.frame_rate);
    
    // 5. 设置图像回调
    auto cam_cfg_copy = cam_cfg_;
    camera_->setImageCallback([this, cam_cfg_copy](const FrameInfo& info) {
        FrameMsg msg;
        
        // 纯 Protobuf 方式填充消息
        msg.set_camera_id(cam_cfg_copy.camera_index);
        msg.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        msg.set_width(info.width);
        msg.set_height(info.height);
        
        uint32_t pixel_type = info.pixelType;
        std::string pixel_format_str;
        
        switch (info.pixelType) {
            case 0x01080001: 
                pixel_type = 1;
                pixel_format_str = "Mono8";
                break;
            case 0x01100001: 
                pixel_type = 2;
                pixel_format_str = "Mono16";
                break;
            case 0x02180001: 
                pixel_type = 3;
                pixel_format_str = "RGB8";
                break;
            case 0x02180008: 
                pixel_type = 4;
                pixel_format_str = "BGR8";
                break;
            case 0x02100002: 
                pixel_type = 5;
                pixel_format_str = "RGB16";
                break;
            case 0x02100010: 
                pixel_type = 6;
                pixel_format_str = "BGR16";
                break;
            case 0x02180002: 
                pixel_type = 7;
                pixel_format_str = "RGBA8";
                break;
            case 0x02180009: 
                pixel_type = 8;
                pixel_format_str = "BGRA8";
                break;
            case 0x01180003: 
                pixel_type = 9;
                pixel_format_str = "YUV422Packed";
                break;
            default:
                pixel_type = 0;
                pixel_format_str = "Unknown";
                break;
        }
        
        msg.set_pixel_type(pixel_type);
        msg.set_frame_num(info.frameNum);
        msg.set_exposure_time(info.exposureTime);
        msg.set_gain(info.gain);
        if (info.data && info.dataLen > 0) {
            msg.set_data(std::string(
                reinterpret_cast<const char*>(info.data),
                info.dataLen
            ));
        }
        LOG_DEBUG("receive frame here: %dx%d, format=%s (SDK=%u, type=%u)", 
                 info.width, info.height, pixel_format_str.c_str(), info.pixelType, pixel_type); 
        if (frame_pub_) frame_pub_->publish(msg);
    });

    // 6. 开始取流
    if (!camera_->startGrabbing()) {
        LOG_ERROR("[CameraNode] 无法开始取流");
        camera_->close();
        return false;
    }

    camera_ready_ = true;
    LOG_INFO("[CameraNode] 相机启动完成，开始采集 (type=%s)", cam_cfg_.camera_type.c_str());
    return true;
}

// ========== stop：关闭相机 ==========
void CameraNode::stop() {
    if (camera_ready_.exchange(false)) {
        if (camera_) {
            camera_->stopGrabbing();
            camera_->close();
            LOG_INFO("[CameraNode] 相机已关闭");
        }
    }
}

// ========== tick：主循环（相机由回调驱动，tick 只做心跳） ==========
void CameraNode::tick(std::atomic<bool>& running) {
    // 相机以回调方式推送帧，这里只进行心跳/健康检查
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ========== 服务端点处理函数 ==========
ServiceResponse CameraNode::handleSetExposure(const ServiceRequest& req) {
    ServiceResponse resp;
    if (!camera_ready_ || !camera_) {
        resp.set_success(false);
        resp.set_data("相机未就绪");
        return resp;
    }
    std::lock_guard<std::mutex> lock(camera_mutex_);
    try {
        float exposure = std::stof(req.payload());
        if (camera_->setExposureTime(exposure)) {
            cam_cfg_.exposure_time = exposure;
            resp.set_success(true);
            resp.set_data("曝光时间已设置: " + std::to_string(static_cast<int>(exposure)));
        } else {
            resp.set_success(false);
            resp.set_data("设置曝光时间失败");
        }
    } catch (const std::exception& e) {
        resp.set_success(false);
        resp.set_data(std::string("参数错误: ") + e.what());
    }
    return resp;
}

ServiceResponse CameraNode::handleSetGain(const ServiceRequest& req) {
    ServiceResponse resp;
    if (!camera_ready_ || !camera_) {
        resp.set_success(false);
        resp.set_data("相机未就绪");
        return resp;
    }
    std::lock_guard<std::mutex> lock(camera_mutex_);
    try {
        float gain = std::stof(req.payload());
        if (camera_->setGain(gain)) {
            cam_cfg_.gain = gain;
            resp.set_success(true);
            resp.set_data("增益已设置: " + std::to_string(gain));
        } else {
            resp.set_success(false);
            resp.set_data("设置增益失败");
        }
    } catch (const std::exception& e) {
        resp.set_success(false);
        resp.set_data(std::string("参数错误: ") + e.what());
    }
    return resp;
}

ServiceResponse CameraNode::handleSetTriggerMode(const ServiceRequest& req) {
    ServiceResponse resp;
    if (!camera_ready_ || !camera_) {
        resp.set_success(false);
        resp.set_data("相机未就绪");
        return resp;
    }
    std::lock_guard<std::mutex> lock(camera_mutex_);
    TriggerMode mode;
    TriggerSource source;
    parseTriggerMode(req.payload(), mode, source);
    camera_->setTriggerMode(mode);
    if (mode == TriggerMode::ON) camera_->setTriggerSource(source);
    cam_cfg_.trigger_mode = req.payload();
    resp.set_success(true);
    resp.set_data("触发模式已设置: " + req.payload());
    return resp;
}

ServiceResponse CameraNode::handleSoftTrigger(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    if (!camera_ready_ || !camera_) {
        resp.set_success(false);
        resp.set_data("相机未就绪");
        return resp;
    }
    std::lock_guard<std::mutex> lock(camera_mutex_);
    if (camera_->triggerSoftware()) {
        resp.set_success(true);
        resp.set_data("软触发成功");
    } else {
        resp.set_success(false);
        resp.set_data("软触发失败");
    }
    return resp;
}

ServiceResponse CameraNode::handleGetConfig(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    std::lock_guard<std::mutex> lock(camera_mutex_);
    std::ostringstream oss;
    oss << "camera_index=" << cam_cfg_.camera_index
        << " exposure_time=" << cam_cfg_.exposure_time
        << " gain=" << cam_cfg_.gain
        << " trigger_mode=" << cam_cfg_.trigger_mode
        << " frame_rate=" << cam_cfg_.frame_rate;
    resp.set_success(true);
    resp.set_data(oss.str());
    return resp;
}
