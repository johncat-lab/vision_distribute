#include "rpc/node_container.h"
#include "rpc/node_factory.h"
#include "rpc/types.h"
#include "logger/logger.h"
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <sstream>

namespace {
// 全局指针，供信号处理器使用
static NodeContainer* g_container = nullptr;
static void nodeContainerSignalHandler(int) {
    if (g_container) {
        g_container->stop();
    }
}
}

NodeContainer::NodeContainer(std::unique_ptr<NodeBase> node)
    : node_(std::move(node)) {
    g_container = this;
}

NodeContainer::~NodeContainer() {
    g_container = nullptr;
}

int NodeContainer::run(int argc, char* argv[]) {
    // 1) --describe 模式：打印 manifest 后退出
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--describe") {
            NodeManifest m = node_->describe();
            std::cout << m.toJsonString() << std::endl;
            return 0;
        }
    }

    // 2) 解析参数
    if (!parseArgs(argc, argv)) {
        std::cerr << "[NodeContainer] 参数解析失败" << std::endl;
        return 1;
    }

    // 3) 安装信号
    std::signal(SIGINT, nodeContainerSignalHandler);
    std::signal(SIGTERM, nodeContainerSignalHandler);

    // 4) 创建传输层（使用默认 NodeConfig）
    NodeConfig cfg;
    cfg.node_name = node_->describe().name;
    cfg.transport = TransportType::ZEROMQ;
    cfg.base_port = 15550;
    factory_ = std::make_unique<NodeFactory>(cfg);

    // 5) 创建 EdgeManager + ServiceEndpointRegistry
    edges_ = std::make_unique<NodeEdgeManager>(*factory_, node_->describe().name);
    // 使用 --instance / --topic-map 配置 EdgeManager
    edges_->parseArgs(argc, argv);

    services_ = std::make_unique<ServiceEndpointRegistry>();

    // 6) 双通道初始化（解耦）
    LOG_INFO("[NodeContainer] 初始化数据流通道: %s",
             node_->describe().name.c_str());
    node_->initDataflow(*edges_, config_file_);

    LOG_INFO("[NodeContainer] 初始化服务通道: %s",
             node_->describe().name.c_str());
    node_->initServices(*services_);

    // 7) 启动
    running_ = true;
    if (!node_->start()) {
        LOG_ERROR("[NodeContainer] 节点启动失败");
        node_->stop();
        return 1;
    }
    LOG_INFO("[NodeContainer] %s 已启动", node_->describe().name.c_str());

    // 8) 主循环：调用节点 tick()
    node_->tick(running_);

    // 9) 停止
    LOG_INFO("[NodeContainer] %s 正在停止...", node_->describe().name.c_str());
    node_->stop();
    running_ = false;

    return 0;
}

void NodeContainer::stop() {
    running_ = false;
}

void NodeContainer::tickOnce() {
    node_->tick(running_);
}

bool NodeContainer::parseArgs(int argc, char* argv[]) {
    // 支持的参数：
    //   --instance <name>       节点实例名
    //   --config <path>         配置文件路径
    //   --topic-map <json>      topic 映射（可选）
    //   --describe              打印 manifest（已在 run() 中处理）
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--instance" && i + 1 < argc) {
            instance_name_ = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_file_ = argv[++i];
        } else if (arg == "--topic-map" && i + 1 < argc) {
            topic_map_ = argv[++i];
        }
    }

    // 默认使用 manifest 中声明的 name 作为 instance
    if (instance_name_.empty()) {
        instance_name_ = node_->describe().name;
    }
    if (config_file_.empty()) {
        config_file_ = node_->describe().config_file;
    }

    LOG_INFO("[NodeContainer] instance=%s, config=%s",
             instance_name_.c_str(), config_file_.c_str());
    return true;
}
