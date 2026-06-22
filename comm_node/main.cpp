#include "rpc/node_factory.h"
#include "rpc/config_loader.h"
#include "rpc/message_types.h"
#include "rpc/node_manifest.h"
#include "rpc/edge_manager.h"
#include "dag/service_registry.h"
#include "vision_server.h"
#include "tcp_client.h"
#include "object_info.h"
#include "logger/logger.h"
#include <string>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <memory>
#include <functional>
#include <csignal>

#include <opencv2/core.hpp>

#ifdef HAS_ROS2
#include "ros2_backend.h"
#include "vision_interfaces/srv/comm_get_config.hpp"
#include "vision_interfaces/srv/comm_get_status.hpp"
#include "vision_interfaces/srv/comm_set_config.hpp"
#endif

// ========== 全局状态 ==========
static std::mutex g_comm_mutex;
static std::unique_ptr<VisionServer> g_server;
static std::unique_ptr<TcpClient> g_client;

// 通信配置
struct CommConfig {
    std::string mode = "server";   // "server" or "client"
    std::string host = "0.0.0.0";
    int port = 7930;
    int server_mode = 2;           // 1=SEND_ON_CONNECT, 2=SEND_ON_REQUEST, 3=SEND_PERIODIC
    int interval_ms = 100;
};

static CommConfig g_config;

// 加载 communication.xml
static CommConfig loadCommConfig(const std::string& path) {
    CommConfig cfg;
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) {
        LOG_WARN("无法打开通信配置文件: %s，使用默认配置", path.c_str());
        return cfg;
    }

    if (!fs["mode"].empty())      cfg.mode = (std::string)fs["mode"];
    if (!fs["host"].empty())      cfg.host = (std::string)fs["host"];
    if (!fs["port"].empty())      cfg.port = (int)fs["port"];
    if (!fs["server_mode"].empty()) cfg.server_mode = (int)fs["server_mode"];
    if (!fs["interval_ms"].empty()) cfg.interval_ms = (int)fs["interval_ms"];

    fs.release();
    return cfg;
}

// 保存通信配置到文件
static bool saveCommConfig(const std::string& path, const CommConfig& cfg) {
    cv::FileStorage fs(path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) {
        LOG_ERROR("无法写入通信配置文件: %s", path.c_str());
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

// ServerMode 枚举转换
static ServerMode intToServerMode(int mode) {
    switch (mode) {
    case 1: return ServerMode::SEND_ON_CONNECT;
    case 3: return ServerMode::SEND_PERIODIC;
    default: return ServerMode::SEND_ON_REQUEST;
    }
}

// 启动通信 (server 或 client 模式)
static void startCommunication(const CommConfig& cfg) {
    std::lock_guard<std::mutex> lock(g_comm_mutex);

    // 先停止旧的
    if (g_server) {
        g_server->stop();
        g_server.reset();
    }
    if (g_client) {
        g_client->disconnect();
        g_client.reset();
    }

    if (cfg.mode == "server") {
        LOG_INFO("启动 Server 模式: %s:%d server_mode=%d", cfg.host.c_str(), cfg.port, cfg.server_mode);
        g_server = std::make_unique<VisionServer>(cfg.port, intToServerMode(cfg.server_mode), cfg.host);
        if (cfg.server_mode == 3) {
            g_server->setInterval(cfg.interval_ms);
        }
        if (!g_server->start()) {
            LOG_ERROR("VisionServer 启动失败");
            g_server.reset();
        }
    } else {
        LOG_INFO("启动 Client 模式: 连接 %s:%d", cfg.host.c_str(), cfg.port);
        g_client = std::make_unique<TcpClient>(cfg.host, cfg.port);
        g_client->enableAutoReconnect(true, 3000);
        g_client->setConnectCallback([](bool connected) {
            LOG_INFO("TcpClient 连接状态: %s", connected ? "已连接" : "已断开");
        });
        if (!g_client->connect()) {
            LOG_WARN("TcpClient 初始连接失败，将自动重连");
        }
    }
}

// 停止通信
static void stopCommunication() {
    std::lock_guard<std::mutex> lock(g_comm_mutex);
    if (g_server) {
        g_server->stop();
        g_server.reset();
    }
    if (g_client) {
        g_client->disconnect();
        g_client.reset();
    }
}

// DetectionMsg -> toFrameString 格式
// protocol_string 已经是 ObjectInfoList::toProtocolString() 格式 (TA,x,y,a,t,...;)
// 直接使用 protocol_string 作为帧字符串
static std::string detectionToFrameString(const DetectionMsg& msg) {
    return msg.protocol_string;
}

// 发送检测结果
static void sendDetectionResult(const std::string& frame_str) {
    std::lock_guard<std::mutex> lock(g_comm_mutex);

    if (g_server) {
        g_server->updateResult(frame_str);
        g_server->broadcast(frame_str);
        LOG_DEBUG("[通信] 检测结果已广播给所有客户端: %s", frame_str.c_str());
    } else if (g_client) {
        if (g_client->isConnected()) {
            g_client->send(frame_str);
            LOG_DEBUG("[通信] 检测结果已发送到服务器: %s", frame_str.c_str());
        }
    }
}

// 获取连接状态
static std::string getConnectionStatus() {
    std::lock_guard<std::mutex> lock(g_comm_mutex);
    if (g_server) {
        return g_server->isRunning() ? "listening" : "disconnected";
    } else if (g_client) {
        return g_client->isConnected() ? "connected" : "disconnected";
    }
    return "disconnected";
}

// 解析 "key=value,key=value" 格式的配置字符串
static CommConfig parseConfigPayload(const std::string& payload, const CommConfig& current) {
    CommConfig cfg = current;
    std::istringstream iss(payload);
    std::string token;
    while (std::getline(iss, token, ',')) {
        size_t eq = token.find('=');
        if (eq == std::string::npos) continue;
        std::string key = token.substr(0, eq);
        std::string val = token.substr(eq + 1);
        if (key == "host")         cfg.host = val;
        else if (key == "port")    cfg.port = std::stoi(val);
        else if (key == "mode")    cfg.mode = val;
        else if (key == "server_mode")  cfg.server_mode = std::stoi(val);
        else if (key == "interval_ms")  cfg.interval_ms = std::stoi(val);
    }
    return cfg;
}

// 配置转字符串
static std::string configToString(const CommConfig& cfg) {
    return "host=" + cfg.host
         + ",port=" + std::to_string(cfg.port)
         + ",mode=" + cfg.mode
         + ",server_mode=" + std::to_string(cfg.server_mode)
         + ",interval_ms=" + std::to_string(cfg.interval_ms);
}

// ========== 信号处理 ==========
static volatile std::sig_atomic_t g_running = 1;
static void signalHandler(int) {
    g_running = 0;
}

static void printUsage(const char* prog) {
    LOG_INFO("用法: %s --config <system_config.xml> --comm-config <communication.xml>", prog);
    LOG_INFO("  --config       系统配置文件路径 (必需)");
    LOG_INFO("  --comm-config  通信配置文件路径 (可选)");
}

// ========== 构建 manifest ==========
static NodeManifest buildManifest() {
    NodeManifest m;
    m.name = "comm_node";
    m.binary = "comm_node";
    m.version = "1.0";
    m.config_file = "communication.xml";
    m.inputs.push_back({"detection_input", "DetectionMsg", "来自检测器的检测结果"});
    m.provides_services.push_back({"comm", {"set_config", "get_config", "get_status"}});
    return m;
}

int main(int argc, char* argv[]) {
    std::string config_path;
    std::string comm_config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--describe") {
            std::cout << buildManifest().toJson() << std::endl;
            return 0;
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--comm-config" && i + 1 < argc) {
            comm_config_path = argv[++i];
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

    NodeConfig config;
    try {
        config = ConfigLoader::loadSystemConfig(config_path);
        config.node_name = "comm_node";
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
    LOG_INFO("Communication Node 启动，传输方式: %s", transport_name.c_str());

    if (!comm_config_path.empty()) {
        g_config = loadCommConfig(comm_config_path);
        LOG_INFO("通信配置: mode=%s host=%s port=%d server_mode=%d interval_ms=%d", 
                 g_config.mode.c_str(), g_config.host.c_str(), g_config.port, 
                 g_config.server_mode, g_config.interval_ms);
    } else {
        LOG_INFO("未指定通信配置文件，使用默认配置 (server, 0.0.0.0:7930)");
    }

    // 3. 创建 NodeFactory + EdgeManager
    NodeFactory factory(config);
    NodeEdgeManager edges(factory, "comm_node");
    edges.setDefaultTopic("detection_input", "vision/detection");
    edges.parseArgs(argc, argv);

    // 4. 创建检测消息订阅者
    auto detection_sub = edges.subscribe<DetectionMsg>("detection_input", "vision/detection");
    LOG_INFO("[CommNode] 检测订阅者已创建，topic: %s", detection_sub->getTopic().c_str());

    // 4b. 创建 ServiceRegistry 并注册 role
    static ServiceRegistry registry(factory);
    const std::string inst_name = edges.instanceName().empty() ? std::string("comm") : edges.instanceName();
    registry.registerRole("comm", inst_name, "comm");
    LOG_INFO("[CommNode] 已注册到 ServiceRegistry: role=comm instance=%s", inst_name.c_str());

    // 5. 创建服务端
    auto service = factory.createService<ServiceRequest, ServiceResponse>("comm");

    // 6. 注册检测回调: 反序列化 DetectionMsg → toFrameString → 发送
    detection_sub->subscribe([](const DetectionMsg& msg) {
        std::string frame_str = detectionToFrameString(msg);
        sendDetectionResult(frame_str);
    });

    // 7. 注册服务端点
#ifdef HAS_ROS2
    // 注册原生 ROS2 service 类型映射（双向转换：服务端+客户端）
    // serve() 会自动创建对应的原生 service，call() 也通过原生 client 调用
    if (auto* rs = dynamic_cast<Ros2Service<ServiceRequest, ServiceResponse>*>(service.get())) {
        rs->registerNativeEndpoint<vision_interfaces::srv::CommSetConfig>(
            "set_config",
            // 服务端: 原生请求 → ServiceRequest
            [](auto req) -> ServiceRequest {
                ServiceRequest sr;
                sr.payload = req->config_data;
                return sr;
            },
            // 服务端: ServiceResponse → 原生响应
            [](const ServiceResponse& sr, auto resp) {
                resp->success = sr.success;
                resp->message = sr.data;
            },
            // 客户端: ServiceRequest → 原生请求
            [](const ServiceRequest& sr) -> std::shared_ptr<vision_interfaces::srv::CommSetConfig::Request> {
                auto req = std::make_shared<vision_interfaces::srv::CommSetConfig::Request>();
                req->config_data = sr.payload;
                return req;
            },
            // 客户端: 原生响应 → ServiceResponse
            [](auto resp) -> ServiceResponse {
                ServiceResponse sr;
                sr.success = resp->success;
                sr.data = resp->message;
                return sr;
            });
        rs->registerNativeEndpoint<vision_interfaces::srv::CommGetConfig>(
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
        rs->registerNativeEndpoint<vision_interfaces::srv::CommGetStatus>(
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
    }
#endif

    service->serve("set_config", [&](const ServiceRequest& req) -> ServiceResponse {
        ServiceResponse resp;
        LOG_DEBUG("收到 set_config 请求: %s", req.payload.c_str());

        CommConfig new_cfg = parseConfigPayload(req.payload, g_config);

        // 检查是否需要重启通信
        bool need_restart = (new_cfg.mode != g_config.mode ||
                             new_cfg.host != g_config.host ||
                             new_cfg.port != g_config.port ||
                             new_cfg.server_mode != g_config.server_mode ||
                             new_cfg.interval_ms != g_config.interval_ms);

        g_config = new_cfg;

        // 保存到文件
        if (!comm_config_path.empty()) {
            saveCommConfig(comm_config_path, g_config);
        }

        if (need_restart) {
            startCommunication(g_config);
        }

        resp.success = true;
        resp.data = "配置已更新: " + configToString(g_config);
        return resp;
    });

    // comm/get_config - 获取当前配置
    service->serve("get_config", [&](const ServiceRequest& /*req*/) -> ServiceResponse {
        ServiceResponse resp;
        resp.success = true;
        resp.data = configToString(g_config);
        return resp;
    });

    // comm/get_status - 获取连接状态
    service->serve("get_status", [&](const ServiceRequest& /*req*/) -> ServiceResponse {
        ServiceResponse resp;
        resp.success = true;
        resp.data = getConnectionStatus();
        return resp;
    });

    // 8. 启动通信
    startCommunication(g_config);

    // 注册信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    LOG_INFO("Communication Node 运行中...");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("Communication Node 正在关闭...");
    stopCommunication();

    return 0;
}
