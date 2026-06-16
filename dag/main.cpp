#include "dag/dag_scheduler.h"
#include "rpc/config_loader.h"
#include "logger/logger.h"

#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <thread>

static std::atomic<bool> g_running{true};
static std::vector<pid_t> g_children;

static void signalHandler(int sig) {
    (void)sig;
    g_running = false;
}

static void printUsage(const char* prog) {
    std::cout << "用法: " << prog << " --pipeline <pipeline.xml> [--bins-dir <path>] [--dry-run]\n";
    std::cout << "\n选项:\n";
    std::cout << "  --pipeline <path>   pipeline.xml 路径（必需）\n";
    std::cout << "  --bins-dir <path>   节点 binary 目录（默认: pipeline.xml 同级）\n";
    std::cout << "  --dry-run           仅打印启动命令，不实际启动\n";
    std::cout << "  --startup-delay <ms> 节点间启动间隔（毫秒，默认 500）\n";
    std::cout << "  --help              显示此帮助\n";
}

int main(int argc, char* argv[]) {
    std::string pipeline_path;
    std::string bins_dir;
    bool dry_run = false;
    int startup_delay_ms = 500;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--pipeline" && i + 1 < argc) {
            pipeline_path = argv[++i];
        } else if (arg == "--bins-dir" && i + 1 < argc) {
            bins_dir = argv[++i];
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--startup-delay" && i + 1 < argc) {
            startup_delay_ms = std::atoi(argv[++i]);
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

    // 默认 bins_dir 为 pipeline.xml 所在目录
    if (bins_dir.empty()) {
        bins_dir = std::filesystem::path(pipeline_path).parent_path().string();
        if (bins_dir.empty()) bins_dir = ".";
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    LOG_INFO("[DagLauncher] pipeline: %s", pipeline_path.c_str());
    LOG_INFO("[DagLauncher] bins-dir: %s", bins_dir.c_str());

    // ========== 加载 + 校验 ==========
    DagScheduler scheduler;
    if (!scheduler.loadFromXml(pipeline_path)) {
        LOG_ERROR("[DagLauncher] 无法加载 pipeline.xml: %s", pipeline_path.c_str());
        return 1;
    }

    std::string validation_error;
    if (!scheduler.validate(validation_error)) {
        LOG_ERROR("[DagLauncher] DAG 校验失败:\n%s", validation_error.c_str());
        return 1;
    }

    auto order = scheduler.computeStartupOrder();
    if (order.empty()) {
        LOG_ERROR("[DagLauncher] DAG 中存在环，无法启动");
        return 1;
    }

    LOG_INFO("[DagLauncher] 启动顺序 (%zu 个节点):", order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        LOG_INFO("  %zu. %s", i + 1, order[i].c_str());
    }

    // ========== dry-run ==========
    if (dry_run) {
        std::cout << "\n=== Dry Run: 启动命令 ===\n\n";
        for (const auto& inst_name : order) {
            std::string binary = scheduler.getBinary(inst_name);
            std::string config = scheduler.getConfig(inst_name);
            std::string topic_map = scheduler.getTopicMap(inst_name);

            std::string binary_path = bins_dir + "/" + binary + "/" + binary;

            std::cout << binary_path;
            if (!config.empty()) {
                std::cout << " --config " << config;
            }
            std::cout << " --instance " << inst_name;
            if (!topic_map.empty()) {
                std::cout << " --topic-map " << topic_map;
            }
            std::cout << "\n\n";
        }
        return 0;
    }

    // ========== 启动节点 ==========
    for (const auto& inst_name : order) {
        if (!g_running) break;

        std::string binary = scheduler.getBinary(inst_name);
        std::string config = scheduler.getConfig(inst_name);
        std::string topic_map = scheduler.getTopicMap(inst_name);

        std::string binary_path = bins_dir + "/" + binary + "/" + binary;

        if (!std::filesystem::exists(binary_path)) {
            LOG_WARN("[DagLauncher] binary 不存在: %s (跳过 %s)", binary_path.c_str(), inst_name.c_str());
            continue;
        }

        // 构建参数列表
        std::vector<std::string> args;
        args.push_back(binary_path);
        if (!config.empty()) {
            args.push_back("--config");
            args.push_back(config);
        }
        args.push_back("--instance");
        args.push_back(inst_name);
        if (!topic_map.empty()) {
            args.push_back("--topic-map");
            args.push_back(topic_map);
        }

        LOG_INFO("[DagLauncher] 启动: %s", inst_name.c_str());
        for (const auto& a : args) {
            LOG_INFO("  %s", a.c_str());
        }

        pid_t pid = fork();
        if (pid < 0) {
            LOG_ERROR("[DagLauncher] fork 失败: %s", strerror(errno));
            continue;
        }

        if (pid == 0) {
            // 子进程
            std::vector<char*> c_args;
            for (auto& a : args) c_args.push_back(const_cast<char*>(a.c_str()));
            c_args.push_back(nullptr);

            execv(c_args[0], c_args.data());
            // execv 失败才到这里
            LOG_ERROR("[DagLauncher] execv 失败: %s (%s)", c_args[0], strerror(errno));
            _exit(1);
        }

        // 父进程
        g_children.push_back(pid);
        LOG_INFO("[DagLauncher] %s PID=%d", inst_name.c_str(), pid);

        // 节点间启动间隔
        if (startup_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(startup_delay_ms));
        }
    }

    // ========== 等待退出信号 ==========
    LOG_INFO("[DagLauncher] 所有节点已启动，等待退出信号...");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 检查子进程状态
        for (auto it = g_children.begin(); it != g_children.end(); ) {
            int status;
            pid_t result = waitpid(*it, &status, WNOHANG);
            if (result > 0) {
                LOG_WARN("[DagLauncher] 子进程 PID=%d 已退出 (status=%d)", result, WEXITSTATUS(status));
                it = g_children.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ========== 清理 ==========
    LOG_INFO("[DagLauncher] 收到退出信号，正在终止子进程...");
    for (auto pid : g_children) {
        kill(pid, SIGTERM);
    }

    // 等待子进程退出（最多 5 秒）
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !g_children.empty()) {
        for (auto it = g_children.begin(); it != g_children.end(); ) {
            int status;
            pid_t result = waitpid(*it, &status, WNOHANG);
            if (result > 0) {
                it = g_children.erase(it);
            } else {
                ++it;
            }
        }
        if (!g_children.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // 强制 kill 剩余的
    for (auto pid : g_children) {
        LOG_WARN("[DagLauncher] 强制 kill PID=%d", pid);
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }

    LOG_INFO("[DagLauncher] 已停止");
    return 0;
}
