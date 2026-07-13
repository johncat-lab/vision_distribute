#include "rpc/node_container.h"
#include "rpc/node_factory.h"
#include "rpc/config_loader.h"
#include "rpc/types.h"
#include "dag/service_registry.h"
#include "logger/logger.h"
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <sstream>
#include <unistd.h>  // getppid()

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
            std::cout << m.toJson() << std::endl;
            return 0;
        }
    }

    // 2) 解析参数
    if (!parseArgs(argc, argv)) {
        std::cerr << "[NodeContainer] 参数解析失败" << std::endl;
        return 1;
    }

    // 2b) 记录父进程 PID (用于检测孤儿化)
    pid_t parent_pid = getppid();
    LOG_INFO("[NodeContainer] 父进程 PID=%d", parent_pid);

    // 3) 安装信号
    std::signal(SIGINT, nodeContainerSignalHandler);
    std::signal(SIGTERM, nodeContainerSignalHandler);

    // 4) 加载系统配置（transport 层）
    NodeConfig cfg;
    cfg.node_name = node_->describe().name;

    // 尝试加载 system_config.xml（优先使用 --system-config 指定的路径，否则查找 CWD 下的文件）
    std::string sys_cfg_path = system_config_;
    if (sys_cfg_path.empty()) {
        // 默认查找当前目录下的 system_config.xml
        if (access("system_config.xml", F_OK) == 0) {
            sys_cfg_path = "system_config.xml";
        }
    }
    if (!sys_cfg_path.empty() && access(sys_cfg_path.c_str(), F_OK) == 0) {
        try {
            cfg = ConfigLoader::loadSystemConfig(sys_cfg_path);
            cfg.node_name = node_->describe().name;
            LOG_INFO("[NodeContainer] 已加载系统配置: %s (transport=%s)",
                     sys_cfg_path.c_str(),
                     cfg.transport == TransportType::ROS2 ? "ROS2" :
                     cfg.transport == TransportType::ZENOH ? "Zenoh" : "ZeroMQ");
        } catch (const std::exception& e) {
            LOG_WARN("[NodeContainer] 加载系统配置失败: %s，回退到 ZeroMQ", e.what());
            cfg.transport = TransportType::ZEROMQ;
            cfg.base_port = 15550;
        }
    } else {
        cfg.transport = TransportType::ZEROMQ;
        cfg.base_port = 15550;
        LOG_INFO("[NodeContainer] 未找到系统配置文件，使用默认 ZeroMQ");
    }

    factory_ = std::make_unique<NodeFactory>(cfg);

    // 4b) 创建 ServiceRegistry（dag 模块，复用 factory_ 的传输层）
    service_registry_ = std::make_unique<ServiceRegistry>(*factory_);

    // 4c) 提前创建主 IService 实例（供 initServices 注册 ROS2 原生类型）
    auto manifest = node_->describe();
    if (!manifest.provides_services.empty()) {
        const auto& svc_info = manifest.provides_services[0];
        service_ = factory_->createService<ServiceRequest, ServiceResponse>(svc_info.role);
        if (service_) {
            LOG_INFO("[NodeContainer] IService 已提前创建: '%s'", svc_info.role.c_str());
        }
    }

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
    node_->initServices(*services_, *this);

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

    // 8) 主循环：调用节点 tick() + 父进程检测
    while (running_) {
        // 检测父进程是否退出 (孤儿化检测)
        pid_t current_parent = getppid();
        if (current_parent != parent_pid) {
            LOG_WARN("[NodeContainer] 检测到父进程已退出 (PID %d -> %d)", 
                     parent_pid, current_parent);
            LOG_WARN("[NodeContainer] 节点将在 3 秒后自动退出...");
            
            // 给 3 秒时间清理
            std::this_thread::sleep_for(std::chrono::seconds(3));
            running_ = false;
            break;
        }
        
        // 执行节点主循环
        node_->tick(running_);
        
        // 短暂休眠，避免 CPU 空转
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

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

std::shared_ptr<IService<ServiceRequest, ServiceResponse>>
NodeContainer::createServiceClient(const std::string& service_name) {
    if (!factory_) {
        LOG_ERROR("[NodeContainer] createServiceClient: factory_ 未初始化");
        return nullptr;
    }
    auto client = factory_->createService<ServiceRequest, ServiceResponse>(service_name);
    if (client) {
        service_instances_.push_back(client);
        LOG_INFO("[NodeContainer] 已创建 service client: '%s'", service_name.c_str());
    } else {
        LOG_ERROR("[NodeContainer] 无法创建 service client: '%s'", service_name.c_str());
    }
    return client;
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
        } else if (arg == "--system-config" && i + 1 < argc) {
            system_config_ = argv[++i];
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

    bool service_reused = false;

    for (const auto& svc_info : manifest.provides_services) {
        const std::string& service_name = svc_info.role;

        // 1) 在 ServiceRegistry（dag 模块）中注册角色，用于跨节点角色发现
        if (service_registry_) {
            service_registry_->registerRole(
                service_name, instance_name_, service_name);
            LOG_INFO("[NodeContainer] 已注册到 ServiceRegistry: role='%s' instance='%s'",
                     service_name.c_str(), instance_name_.c_str());
        }

        // 2) 获取或创建 IService 网络传输实例
        //    主服务已在 run() 中提前创建（供 initServices 注册 ROS2 原生类型），
        //    此处复用；其他角色创建新实例。
        std::shared_ptr<IService<ServiceRequest, ServiceResponse>> service;
        if (service_ && !service_reused) {
            service = service_;
            service_reused = true;
        } else {
            service = factory_->createService<ServiceRequest, ServiceResponse>(
                service_name);
            if (!service) {
                LOG_ERROR("[NodeContainer] 无法创建 service: '%s'", service_name.c_str());
                continue;
            }
            service_instances_.push_back(service);
        }

        // 3) 将该 service 的所有端点桥接到 ServiceEndpointRegistry::handle()
        //    网络请求 → IService::serve() → ServiceEndpointRegistry::handle()
        //             → 权限检查 → 限流检查 → 实际 handler
        //
        //    ROS2 传输下，端点的原生类型已在 initServices() 中通过
        //    registerRos2NativeEndpoint<SrvType>() 注册，serve() 会自动
        //    创建对应的原生 ROS2 service。
        int endpoint_count = 0;
        for (const auto& ep_name : svc_info.endpoints) {
            try {
                service->serve(ep_name,
                    [this, ep_name](const ServiceRequest& req) -> ServiceResponse {
                        // 将请求转发给 ServiceEndpointRegistry 统一处理
                        ServiceRequest routed_req = req;
                        routed_req.set_endpoint(ep_name);
                        return services_->handle(ep_name, routed_req);
                    });
                endpoint_count++;
            } catch (const std::exception& e) {
                LOG_WARN("[NodeContainer] 端点桥接失败: %s/%s (%s)",
                         service_name.c_str(), ep_name.c_str(), e.what());
            }
        }

        LOG_INFO("[NodeContainer] 服务桥接完成: service='%s' 端点数=%d",
                 service_name.c_str(), endpoint_count);
    }

    // 确保主服务被持有（service_instances_ 管理生命周期）
    if (service_) {
        service_instances_.push_back(service_);
    }
}
