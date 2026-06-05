#include "rpc/node_factory.h"
#include "rpc/config_loader.h"
#include "rpc/message_types.h"
#include "vision_server.h"
#include "tcp_client.h"
#include "object_info.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <memory>
#include <functional>
#include <csignal>

#include <opencv2/core.hpp>

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
        std::cerr << "警告: 无法打开通信配置文件: " << path << "，使用默认配置" << std::endl;
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
        std::cerr << "错误: 无法写入通信配置文件: " << path << std::endl;
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
        std::cout << "启动 Server 模式: " << cfg.host << ":" << cfg.port
                  << " server_mode=" << cfg.server_mode << std::endl;
        g_server = std::make_unique<VisionServer>(cfg.port, intToServerMode(cfg.server_mode), cfg.host);
        if (cfg.server_mode == 3) {
            g_server->setInterval(cfg.interval_ms);
        }
        if (!g_server->start()) {
            std::cerr << "错误: VisionServer 启动失败" << std::endl;
            g_server.reset();
        }
    } else {
        std::cout << "启动 Client 模式: 连接 " << cfg.host << ":" << cfg.port << std::endl;
        g_client = std::make_unique<TcpClient>(cfg.host, cfg.port);
        g_client->enableAutoReconnect(true, 3000);
        g_client->setConnectCallback([](bool connected) {
            std::cout << "TcpClient 连接状态: " << (connected ? "已连接" : "已断开") << std::endl;
        });
        if (!g_client->connect()) {
            std::cerr << "警告: TcpClient 初始连接失败，将自动重连" << std::endl;
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
        // Server 模式: 更新结果，由 Server 按模式发送
        g_server->updateResult(frame_str);
    } else if (g_client) {
        // Client 模式: 主动发送
        if (g_client->isConnected()) {
            g_client->send(frame_str);
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
    std::cout << "用法: " << prog << " --config <system_config.xml> --comm-config <communication.xml>" << std::endl;
    std::cout << "  --config       系统配置文件路径 (必需)" << std::endl;
    std::cout << "  --comm-config  通信配置文件路径 (可选)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string config_path;
    std::string comm_config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--comm-config" && i + 1 < argc) {
            comm_config_path = argv[++i];
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

    // 1. 加载系统配置
    NodeConfig config;
    try {
        config = ConfigLoader::loadSystemConfig(config_path);
        config.node_name = "comm_node";
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
    std::cout << "Communication Node 启动，传输方式: " << transport_name << std::endl;

    // 2. 加载通信配置
    if (!comm_config_path.empty()) {
        g_config = loadCommConfig(comm_config_path);
        std::cout << "通信配置: mode=" << g_config.mode
                  << " host=" << g_config.host
                  << " port=" << g_config.port
                  << " server_mode=" << g_config.server_mode
                  << " interval_ms=" << g_config.interval_ms << std::endl;
    } else {
        std::cout << "未指定通信配置文件，使用默认配置 (server, 0.0.0.0:7930)" << std::endl;
    }

    // 3. 创建 NodeFactory
    NodeFactory factory(config);

    // 4. 创建检测消息订阅者
    auto detection_sub = factory.createSubscriber<DetectionMsg>("vision/detection");

    // 5. 创建服务端
    auto service = factory.createService<ServiceRequest, ServiceResponse>("comm");

    // 6. 注册检测回调: 反序列化 DetectionMsg → toFrameString → 发送
    detection_sub->subscribe([](const DetectionMsg& msg) {
        std::string frame_str = detectionToFrameString(msg);
        sendDetectionResult(frame_str);
    });

    // 7. 注册服务端点
    // comm/set_config - 动态修改配置
    service->serve("comm/set_config", [&](const ServiceRequest& req) -> ServiceResponse {
        ServiceResponse resp;
        std::cout << "收到 set_config 请求: " << req.payload << std::endl;

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
    service->serve("comm/get_config", [&](const ServiceRequest& /*req*/) -> ServiceResponse {
        ServiceResponse resp;
        resp.success = true;
        resp.data = configToString(g_config);
        return resp;
    });

    // comm/get_status - 获取连接状态
    service->serve("comm/get_status", [&](const ServiceRequest& /*req*/) -> ServiceResponse {
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

    std::cout << "Communication Node 运行中..." << std::endl;

    // 9. 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 清理
    std::cout << "Communication Node 正在关闭..." << std::endl;
    stopCommunication();

    return 0;
}
