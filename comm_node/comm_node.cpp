#include "comm_node.h"
#include "rpc/node_factory.h"
#include "rpc/edge_manager.h"
#include "rpc/service_endpoint_registry.h"
#include "logger/logger.h"

#ifdef HAS_ROS2
#include "vision_interfaces/srv/comm_set_config.hpp"
#include "vision_interfaces/srv/comm_get_config.hpp"
#include "vision_interfaces/srv/comm_get_status.hpp"
#endif

#include <opencv2/core.hpp>
#include <filesystem>
#include <sstream>
#include <thread>
#include <chrono>

CommNode::~CommNode() {
    stop();
}

// ========== 配置加载 ==========
CommNode::CommConfig CommNode::loadConfig(const std::string& path) {
    CommConfig cfg;
    if (path.empty()) return cfg;
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        LOG_WARN("[CommNode] 无法打开通信配置文件: %s", path.c_str());
        return cfg;
    }
    if (!fs["mode"].empty())          cfg.mode = (std::string)fs["mode"];
    if (!fs["host"].empty())          cfg.host = (std::string)fs["host"];
    if (!fs["port"].empty())          cfg.port = (int)fs["port"];
    if (!fs["server_mode"].empty()) cfg.server_mode = (int)fs["server_mode"];
    if (!fs["interval_ms"].empty()) cfg.interval_ms = (int)fs["interval_ms"];
    fs.release();
    return cfg;
}

bool CommNode::saveConfig(const std::string& path, const CommConfig& cfg) {
    cv::FileStorage fs(path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) {
        LOG_ERROR("[CommNode] 无法写入配置: %s", path.c_str());
        return false;
    }
    fs << "mode" << cfg.mode;
    fs << "host" << cfg.host;
    fs << "port" << cfg.port;
    fs << "server_mode" << cfg.server_mode;
    fs << "interval_ms" << cfg.interval_ms;
    fs.release();
    return true;
}

// ========== manifest ==========
NodeManifest CommNode::describe() const {
    NodeManifest m;
    m.name = "comm_node";
    m.binary = "comm_node";
    m.version = "1.0";
    m.config_file = "communication.xml";
    m.inputs.push_back({"detection_input", "DetectionMsg", "检测结果"});
    m.provides_services.push_back({"comm", {"set_config", "get_config", "get_status"}});
    return m;
}

// ========== initDataflow ==========
void CommNode::initDataflow(NodeEdgeManager& edges,
                               const std::string& config_file) {
    edges.setDefaultTopic("detection_input", "vision/detection");
    detection_sub_ = edges.subscribe<DetectionMsg>("detection_input", "vision/detection");

    detection_sub_->subscribe([this](const DetectionMsg& msg) {
        std::lock_guard<std::mutex> lock(detection_mutex_);
        latest_detection_ = msg;
        (void)msg;
    });

    // 保存配置文件路径供 start() 使用
    comm_config_file_ = config_file;
    
    if (!comm_config_file_.empty()) {
        LOG_INFO("[CommNode] 通信配置文件: %s", comm_config_file_.c_str());
    } else {
        LOG_INFO("[CommNode] 未指定通信配置文件，将使用默认值");
    }

    LOG_INFO("[CommNode] 数据流通道已初始化");
}

// ========== initServices ==========
void CommNode::initServices(ServiceEndpointRegistry& services) {
    services.registerEndpoint({"set_config", "设置通信配置", false, 0},
        [this](const ServiceRequest& req) { return handleSetConfig(req); });

    services.registerEndpoint({"get_config", "获取当前通信配置", false, 0},
        [this](const ServiceRequest& req) { return handleGetConfig(req); });

    services.registerEndpoint({"get_status", "获取连接状态", false, 0},
        [this](const ServiceRequest& req) { return handleGetStatus(req); });

    LOG_INFO("[CommNode] 已注册 3 个服务端点");
}

// ========== initServices (ROS2 原生类型注册) ==========
void CommNode::initServices(ServiceEndpointRegistry& services, NodeContainer& container) {
    // 先注册通用端点处理函数
    initServices(services);

#ifdef HAS_ROS2
    // 注册 ROS2 原生 .srv 类型映射
    container.registerRos2NativeEndpoint<vision_interfaces::srv::CommSetConfig>(
        "set_config",
        [](auto req) -> ServiceRequest {
            ServiceRequest sr;
            sr.payload = req->config_data;
            return sr;
        },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success;
            resp->message = sr.data;
        },
        [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CommSetConfig::Request> {
            auto req = std::make_shared<vision_interfaces::srv::CommSetConfig::Request>();
            req->config_data = sr.payload;
            return req;
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.success = resp->success;
            sr.data = resp->message;
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::CommGetConfig>(
        "get_config",
        [](auto) -> ServiceRequest { return ServiceRequest{}; },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success;
            resp->config_data = sr.data;
        },
        [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CommGetConfig::Request> {
            return std::make_shared<vision_interfaces::srv::CommGetConfig::Request>();
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.success = resp->success;
            sr.data = resp->config_data;
            return sr;
        });

    container.registerRos2NativeEndpoint<vision_interfaces::srv::CommGetStatus>(
        "get_status",
        [](auto) -> ServiceRequest { return ServiceRequest{}; },
        [](const ServiceResponse& sr, auto resp) {
            resp->success = sr.success;
            resp->status_data = sr.data;
        },
        [](const ServiceRequest&) -> std::shared_ptr<vision_interfaces::srv::CommGetStatus::Request> {
            return std::make_shared<vision_interfaces::srv::CommGetStatus::Request>();
        },
        [](auto resp) -> ServiceResponse {
            ServiceResponse sr;
            sr.success = resp->success;
            sr.data = resp->status_data;
            return sr;
        });

    LOG_INFO("[CommNode] 已注册 3 个 ROS2 原生 service 类型映射");
#endif
}

// ========== start ==========
bool CommNode::start() {
    // 使用 initDataflow 中保存的配置文件路径
    cfg_ = loadConfig(comm_config_file_);

    try {
        if (cfg_.mode == "client") {
            client_ = std::make_unique<TcpClient>(cfg_.host, cfg_.port);
            if (client_) client_->connect();
            LOG_INFO("[CommNode] TCP 客户端已启动 %s:%d",
                     cfg_.host.c_str(), cfg_.port);
        } else {
            server_ = std::make_unique<VisionServer>(cfg_.port);
            if (server_) server_->start();
            LOG_INFO("[CommNode] TCP 服务器已启动 port=%d", cfg_.port);
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("[CommNode] 启动失败: %s", e.what());
        return false;
    }
}

// ========== stop ==========
void CommNode::stop() {
    if (server_) { server_->stop(); server_.reset(); }
    if (client_) { client_->disconnect(); client_.reset(); }
    LOG_INFO("[CommNode] 已停止");
}

// ========== tick：在主循环中按 interval_ms 频率向外部推送结果 ==========
void CommNode::tick(std::atomic<bool>& running) {
    int interval = std::max(10, cfg_.interval_ms);
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));

        if (latest_detection_.object_count() > 0) {
            std::string result;
            {
                std::lock_guard<std::mutex> lock(detection_mutex_);
                std::ostringstream oss;
                oss << "frame=" << latest_detection_.frame_num()
                     << " objects=" << latest_detection_.object_count()
                     << " protocol=" << latest_detection_.protocol_string();
                result = oss.str();
            }
            std::lock_guard<std::mutex> lock(comm_mutex_);
            if (server_) {
                server_->updateResult(result);
                if (cfg_.server_mode == 2) {
                    LOG_DEBUG("[CommNode] 更新检测结果(请求模式): %s", result.c_str());
                } else if (cfg_.server_mode == 3) {
                    server_->broadcast(result + "\n");
                    LOG_DEBUG("[CommNode] 通过服务器广播检测结果(周期模式): %s", result.c_str());
                }
            }
            if (client_) {
                client_->send(result + "\n");
                LOG_DEBUG("[CommNode] 通过客户端发送检测结果: %s", result.c_str());
            }
        }
    }
}

// ========== 服务端点处理 ==========
ServiceResponse CommNode::handleSetConfig(const ServiceRequest& req) {
    ServiceResponse resp;
    resp.set_success(true);
    resp.set_data("配置已更新: " + req.payload());
    return resp;
}

ServiceResponse CommNode::handleGetConfig(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    std::ostringstream oss;
    oss << "mode=" << cfg_.mode << " host=" << cfg_.host
        << " port=" << cfg_.port << " interval_ms=" << cfg_.interval_ms;
    resp.set_success(true);
    resp.set_data(oss.str());
    return resp;
}

ServiceResponse CommNode::handleGetStatus(const ServiceRequest& req) {
    (void)req;
    ServiceResponse resp;
    resp.set_success(true);
    std::ostringstream oss;
    oss << "mode=" << cfg_.mode;
    if (server_) {
        oss << " server_running=1 clients=" << server_->getClientCount();
    }
    if (client_) {
        oss << " client_connected=" << (client_->isConnected() ? 1 : 0);
    }
    resp.set_data(oss.str());
    return resp;
}
