#pragma once
#include "dag/dag_scheduler.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <functional>
#include <condition_variable>
#include <mutex>

struct NodeLaunchConfig {
    std::string instance_name;
    std::string binary_path;
    std::string config_file;
    std::string topic_map;
};

enum class NodeRuntimeStatus {
    PENDING,
    STARTING,
    RUNNING,
    EXITED,
    CRASHED,
    FAILED
};

struct NodeRuntimeState {
    NodeRuntimeStatus status = NodeRuntimeStatus::PENDING;
    int pid = 0;
    int exit_code = 0;
    std::string error_msg;
    int restart_count = 0;
    std::chrono::steady_clock::time_point last_start_time;
};

/// @brief 节点资源用量
struct NodeResourceUsage {
    double cpu_percent = 0.0;   // 0-100（单核）
    long memory_kb = 0;          // 物理内存 KB
    long vm_kb = 0;              // 虚拟内存 KB
};

/// @brief pipeline 热更新差异
struct PipelineDiff {
    std::vector<std::string> to_stop;     // 需要停止的节点
    std::vector<std::string> to_start;    // 需要新启动的节点
    std::vector<std::string> to_restart;  // 配置变更，需重启
    bool changed = false;
};

class DagLauncher {
public:
    using StatusCallback = std::function<void(const std::string&,
                                               NodeRuntimeStatus,
                                               const std::string&)>;

    DagLauncher();
    ~DagLauncher();

    bool loadPipeline(const std::string& pipeline_path, const std::string& bins_dir = "");
    void setStatusCallback(StatusCallback cb) { status_cb_ = std::move(cb); }

    bool validatePipeline(std::string& error_msg) const;
    std::vector<std::string> getStartupOrder() const;
    std::vector<std::vector<std::string>> getStartupLayers() const;
    std::map<std::string, NodeLaunchConfig> getLaunchConfigs() const;

    /// @brief 顺序启动（保持原有行为）
    /// @param startup_delay_ms 节点间启动间隔
    /// @return true=所有节点成功启动
    bool launch(int startup_delay_ms = 200);

    /// @brief 按层并行启动：同层节点并行启动，层间串行
    /// @param inter_layer_delay_ms 层间等待时间
    /// @return true=所有节点成功启动（或重试后成功）
    bool launchParallel(int inter_layer_delay_ms = 200);

    /// @brief 设置启动失败重试次数（默认 3）
    void setMaxRetries(int n) { max_retries_ = n; }

    /// @brief 设置每次重试之间的等待时间（毫秒，默认 1000）
    void setRetryDelayMs(int ms) { retry_delay_ms_ = ms; }

    /// @brief 设置崩溃后是否自动重启（默认 true）
    void setAutoRestart(bool enable) { auto_restart_ = enable; }

    /// @brief 设置每个节点的最大重启次数（默认 10）
    void setMaxRestarts(int n) { max_restarts_ = n; }

    /// @brief 设置自动重启间隔（毫秒，默认 2000）
    void setAutoRestartDelayMs(int ms) { auto_restart_delay_ms_ = ms; }

    /// @brief 运行时重新加载 pipeline，只启停变化的节点
    /// @param new_pipeline_path 新的 pipeline.xml 路径；为空则重载当前的
    /// @param diff 输出差异详情
    /// @return true=成功
    bool reloadPipeline(const std::string& new_pipeline_path, PipelineDiff& diff);

    /// @brief 获取指定节点的资源用量
    NodeResourceUsage getResourceUsage(const std::string& instance_name) const;

    /// @brief 获取所有节点的资源用量
    std::map<std::string, NodeResourceUsage> getAllResourceUsage() const;

    /// @brief 打印当前所有节点状态（调试）
    std::string getStatusReport() const;

    bool allRunning() const;
    void waitForAll();
    void stop();
    void forceKill();
    NodeRuntimeStatus getStatus(const std::string& instance_name) const;

    std::map<std::string, std::vector<std::string>> getRoleProviders() const;
    std::map<std::string, std::vector<std::string>> getRoleRequirements() const;

private:
    bool launchNode(const NodeLaunchConfig& config, NodeRuntimeState& state);
    bool restartNode(const std::string& instance_name);
    bool stopNode(const std::string& instance_name, int signal = 15);
    NodeLaunchConfig buildConfig(const std::string& instance_name) const;
    void monitorThreadFunc();

    PipelineDiff computeDiff(const DagScheduler& new_scheduler) const;

    DagScheduler scheduler_;
    std::string bins_dir_;
    std::string pipeline_path_;
    int max_retries_ = 3;
    int retry_delay_ms_ = 1000;
    bool auto_restart_ = true;
    int max_restarts_ = 10;
    int auto_restart_delay_ms_ = 2000;

    std::map<std::string, NodeLaunchConfig> launch_configs_;
    std::map<std::string, NodeRuntimeState> states_;

    mutable std::mutex states_mutex_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> monitor_thread_;
    StatusCallback status_cb_;
};
