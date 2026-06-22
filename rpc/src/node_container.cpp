#include "rpc/node_container.h"
#include "rpc/node_factory.h"
#include "rpc/types.h"
#include "dag/service_registry.h"
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

    // 4b) 创建 ServiceRegistry（dag 模块，复用 factory_ 的传输层）
    service_registry_ = std::make_unique<ServiceRegistry>(*factory_);

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

    // 6b) 将 ServiceEndpointRegistry 端点桥接到 IService 网络传输层
    bridgeServices();

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

// ========== bridgeServices ==========
// 将 ServiceEndpointRegistry 中注册的端点桥接到 IService 网络传输层，
// 并在 ServiceRegistry（dag 模块）中注册角色，使外部可通过网络调用端点。
void NodeContainer::bridgeServices() {
    auto manifest = node_->describe();
    if (manifest.provides_services.empty()) {
        LOG_INFO("[NodeContainer] %s 无 provides_services，跳过服务桥接",
                 manifest.name.c_str());
        return;
    }

    for (const auto& svc_info : manifest.provides_services) {
        const std::string& service_name = svc_info.role;

        // 1) 在 ServiceRegistry（dag 模块）中注册角色，用于跨节点角色发现
        if (service_registry_) {
            service_registry_->registerRole(
                service_name, instance_name_, service_name);
            LOG_INFO("[NodeContainer] 已注册到 ServiceRegistry: role='%s' instance='%s'",
                     service_name.c_str(), instance_name_.c_str());
        }

        // 2) 创建 IService 网络传输实例
        auto service = factory_->createService<ServiceRequest, ServiceResponse>(
            service_name);
        if (!service) {
            LOG_ERROR("[NodeContainer] 无法创建 service: '%s'", service_name.c_str());
            continue;
        }

        // 3) 将该 service 的所有端点桥接到 ServiceEndpointRegistry::handle()
        //    网络请求 → IService::serve() → ServiceEndpointRegistry::handle()
        //             → 权限检查 → 限流检查 → 实际 handler
        int endpoint_count = 0;
        for (const auto& ep_name : svc_info.endpoints) {
            service->serve(ep_name,
                [this, ep_name](const ServiceRequest& req) -> ServiceResponse {
                    // 将请求转发给 ServiceEndpointRegistry 统一处理
                    ServiceRequest routed_req = req;
                    routed_req.endpoint = ep_name;
                    return services_->handle(ep_name, routed_req);
                });
            endpoint_count++;
        }

        LOG_INFO("[NodeContainer] 服务桥接完成: service='%s' 端点数=%d",
                 service_name.c_str(), endpoint_count);

        // 4) 持有 IService 实例，保持服务线程存活
        service_instances_.push_back(service);
    }
}
