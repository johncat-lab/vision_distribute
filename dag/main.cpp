#include "dag/dag_launcher.h"
#include "logger/logger.h"

#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>

static std::atomic<bool> g_running{true};
static DagLauncher* g_launcher = nullptr;

static void signalHandler(int sig) {
    (void)sig;
    g_running = false;
    if (g_launcher) {
        g_launcher->stop();
    }
}

static void printUsage(const char* prog) {
    std::cout << "用法: " << prog << " --pipeline <pipeline.xml> [--bins-dir <path>] [--parallel] [--dry-run]\n";
    std::cout << "\n选项:\n";
    std::cout << "  --pipeline <path>   pipeline.xml 路径（必需）\n";
    std::cout << "  --bins-dir <path>   节点 binary 目录（默认: pipeline.xml 同级）\n";
    std::cout << "  --parallel          按层并行启动（同层节点无依赖，同时启动）\n";
    std::cout << "  --dry-run           仅打印启动命令，不实际启动\n";
    std::cout << "  --startup-delay <ms> 节点间/层间启动间隔（毫秒，默认 200）\n";
    std::cout << "  --max-retries <n>   启动失败重试次数（默认 3）\n";
    std::cout << "  --retry-delay <ms>  每次重试的等待时间（毫秒，默认 1000）\n";
    std::cout << "  --no-auto-restart   关闭崩溃自动重启功能（默认开启）\n";
    std::cout << "  --max-restarts <n>  每个节点的最大重启次数（默认 10）\n";
    std::cout << "  --auto-restart-delay <ms> 自动重启的等待时间（毫秒，默认 2000）\n";
    std::cout << "  --status-report     启动过程中定期打印状态报告（调试用）\n";
    std::cout << "  --help              显示此帮助\n";
}

int main(int argc, char* argv[]) {
    std::string pipeline_path;
    std::string bins_dir;
    bool dry_run = false;
    bool parallel = false;
    int startup_delay_ms = 200;
    int max_retries = 3;
    int retry_delay_ms = 1000;
    bool auto_restart = true;
    int max_restarts = 10;
    int auto_restart_delay_ms = 2000;
    bool status_report = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--pipeline" && i + 1 < argc) {
            pipeline_path = argv[++i];
        } else if (arg == "--bins-dir" && i + 1 < argc) {
            bins_dir = argv[++i];
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--parallel") {
            parallel = true;
        } else if (arg == "--startup-delay" && i + 1 < argc) {
            startup_delay_ms = std::atoi(argv[++i]);
        } else if (arg == "--max-retries" && i + 1 < argc) {
            max_retries = std::atoi(argv[++i]);
        } else if (arg == "--retry-delay" && i + 1 < argc) {
            retry_delay_ms = std::atoi(argv[++i]);
        } else if (arg == "--no-auto-restart") {
            auto_restart = false;
        } else if (arg == "--max-restarts" && i + 1 < argc) {
            max_restarts = std::atoi(argv[++i]);
        } else if (arg == "--auto-restart-delay" && i + 1 < argc) {
            auto_restart_delay_ms = std::atoi(argv[++i]);
        } else if (arg == "--status-report") {
            status_report = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (pipeline_path.empty()) {
        std::cerr << "错误: 必须指定 --pipeline <pipeline.xml>\n";
        printUsage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    DagLauncher launcher;
    g_launcher = &launcher;
    launcher.setMaxRetries(max_retries);
    launcher.setRetryDelayMs(retry_delay_ms);
    launcher.setAutoRestart(auto_restart);
    launcher.setMaxRestarts(max_restarts);
    launcher.setAutoRestartDelayMs(auto_restart_delay_ms);

    if (!launcher.loadPipeline(pipeline_path, bins_dir)) {
        return 1;
    }

    std::string error_msg;
    if (!launcher.validatePipeline(error_msg)) {
        LOG_ERROR("[DagLauncher] DAG 校验失败:\n%s", error_msg.c_str());
        return 1;
    }

    auto order = launcher.getStartupOrder();
    if (order.empty()) {
        LOG_ERROR("[DagLauncher] 启动顺序为空（可能存在环）");
        return 1;
    }

    LOG_INFO("[DagLauncher] 启动顺序 (%zu 个节点):", order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        LOG_INFO("  %zu. %s", i + 1, order[i].c_str());
    }

    // 打印分层信息
    auto layers = launcher.getStartupLayers();
    if (!layers.empty()) {
        LOG_INFO("[DagLauncher] 分层信息 (%zu 层):", layers.size());
        for (size_t li = 0; li < layers.size(); ++li) {
            std::string s;
            for (size_t ni = 0; ni < layers[li].size(); ++ni) {
                if (ni > 0) s += ",";
                s += layers[li][ni];
            }
            LOG_INFO("  层 %zu: [%s]", li, s.c_str());
        }
    }

    // 打印 service 依赖关系
    auto providers = launcher.getRoleProviders();
    auto requirements = launcher.getRoleRequirements();
    if (!providers.empty()) {
        LOG_INFO("[DagLauncher] Service 角色提供者:");
        for (const auto& [role, insts] : providers) {
            std::string s;
            for (size_t i = 0; i < insts.size(); ++i) {
                if (i > 0) s += ",";
                s += insts[i];
            }
            LOG_INFO("  %s -> [%s]", role.c_str(), s.c_str());
        }
    }
    if (!requirements.empty()) {
        LOG_INFO("[DagLauncher] Service 依赖:");
        for (const auto& [name, roles] : requirements) {
            std::string s;
            for (size_t i = 0; i < roles.size(); ++i) {
                if (i > 0) s += ",";
                s += roles[i];
            }
            LOG_INFO("  %s requires [%s]", name.c_str(), s.c_str());
        }
    }

    if (dry_run) {
        std::cout << "\n=== Dry Run: 启动命令 ===\n\n";
        auto configs = launcher.getLaunchConfigs();
        for (const auto& inst_name : order) {
            auto it = configs.find(inst_name);
            if (it == configs.end()) continue;
            std::cout << it->second.binary_path;
            if (!it->second.config_file.empty()) {
                std::cout << " --config " << it->second.config_file;
            }
            std::cout << " --instance " << inst_name;
            if (!it->second.topic_map.empty()) {
                std::cout << " --topic-map " << it->second.topic_map;
            }
            std::cout << "\n\n";
        }
        return 0;
    }

    // 设置状态回调
    launcher.setStatusCallback([](const std::string& name,
                                    NodeRuntimeStatus status,
                                    const std::string& err) {
        const char* status_str = "UNKNOWN";
        switch (status) {
            case NodeRuntimeStatus::PENDING:  status_str = "PENDING";  break;
            case NodeRuntimeStatus::STARTING: status_str = "STARTING"; break;
            case NodeRuntimeStatus::RUNNING:  status_str = "RUNNING";  break;
            case NodeRuntimeStatus::EXITED:   status_str = "EXITED";   break;
            case NodeRuntimeStatus::CRASHED:  status_str = "CRASHED";  break;
            case NodeRuntimeStatus::FAILED:   status_str = "FAILED";   break;
        }
        if (!err.empty()) {
            LOG_INFO("[DagLauncher] %s 状态: %s (%s)", name.c_str(), status_str, err.c_str());
        } else {
            LOG_INFO("[DagLauncher] %s 状态: %s", name.c_str(), status_str);
        }
    });

    // 启动所有节点
    if (parallel) {
        LOG_INFO("[DagLauncher] 使用按层并行启动模式");
        launcher.launchParallel(startup_delay_ms);
    } else {
        LOG_INFO("[DagLauncher] 使用顺序启动模式");
        launcher.launch(startup_delay_ms);
    }
    LOG_INFO("[DagLauncher] 所有节点启动完成，等待退出信号...");

    // 主循环：等待退出信号
    int report_counter = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (status_report) {
            report_counter++;
            if (report_counter % 20 == 0) {
                std::cout << launcher.getStatusReport() << std::endl;
            }
        }
    }

    // 优雅退出：等待所有子进程清理完成
    LOG_INFO("[DagLauncher] 收到退出信号，正在清理...");
    launcher.stop();  // 这会等待所有子进程退出（超时5秒）
    
    LOG_INFO("[DagLauncher] 已停止");
    return 0;
}
