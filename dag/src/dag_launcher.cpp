#include "dag/dag_launcher.h"
#include "logger/logger.h"
#include <filesystem>
#include <sstream>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <future>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

DagLauncher::DagLauncher() = default;

DagLauncher::~DagLauncher() {
    if (running_.load()) {
        stop();
    }
    if (monitor_thread_ && monitor_thread_->joinable()) {
        monitor_thread_->join();
    }
}

bool DagLauncher::loadPipeline(const std::string& pipeline_path, const std::string& bins_dir) {
    pipeline_path_ = pipeline_path;
    bins_dir_ = bins_dir;

    if (!scheduler_.loadFromXml(pipeline_path_)) {
        LOG_ERROR("[DagLauncher] 无法加载 pipeline.xml: %s", pipeline_path_.c_str());
        return false;
    }

    if (bins_dir_.empty()) {
        fs::path p(pipeline_path_);
        if (p.has_parent_path()) {
            bins_dir_ = p.parent_path().string();
        } else {
            bins_dir_ = ".";
        }
    }

    auto order = scheduler_.computeStartupOrder();
    for (const auto& inst : order) {
        launch_configs_[inst] = buildConfig(inst);
        states_[inst] = NodeRuntimeState();
    }

    LOG_INFO("[DagLauncher] pipeline 已加载: %s", pipeline_path_.c_str());
    LOG_INFO("[DagLauncher] bins-dir: %s", bins_dir_.c_str());
    LOG_INFO("[DagLauncher] 共 %zu 个节点", launch_configs_.size());
    return true;
}

bool DagLauncher::validatePipeline(std::string& error_msg) const {
    return scheduler_.validate(error_msg);
}

std::vector<std::string> DagLauncher::getStartupOrder() const {
    return scheduler_.computeStartupOrder();
}

std::vector<std::vector<std::string>> DagLauncher::getStartupLayers() const {
    return scheduler_.computeStartupLayers();
}

std::map<std::string, NodeLaunchConfig> DagLauncher::getLaunchConfigs() const {
    return launch_configs_;
}

NodeLaunchConfig DagLauncher::buildConfig(const std::string& instance_name) const {
    NodeLaunchConfig cfg;
    cfg.instance_name = instance_name;
    std::string binary = scheduler_.getBinary(instance_name);
    if (!binary.empty()) {
        fs::path bp = fs::path(bins_dir_) / binary / binary;
        cfg.binary_path = bp.string();
    }
    cfg.config_file = scheduler_.getConfig(instance_name);
    cfg.topic_map = scheduler_.getTopicMap(instance_name);
    return cfg;
}

bool DagLauncher::launch(int startup_delay_ms) {
    auto order = getStartupOrder();
    if (order.empty()) {
        LOG_ERROR("[DagLauncher] 启动顺序为空（可能存在环）");
        return false;
    }

    running_ = true;
    monitor_thread_ = std::make_unique<std::thread>([this]() {
        this->monitorThreadFunc();
    });

    bool all_ok = true;
    for (const auto& inst_name : order) {
        if (!running_.load()) break;

        auto cfg_it = launch_configs_.find(inst_name);
        if (cfg_it == launch_configs_.end()) {
            LOG_ERROR("[DagLauncher] 找不到配置: %s", inst_name.c_str());
            all_ok = false;
            continue;
        }

        if (!fs::exists(cfg_it->second.binary_path)) {
            LOG_WARN("[DagLauncher] binary 不存在: %s (跳过 %s)",
                     cfg_it->second.binary_path.c_str(), inst_name.c_str());
            continue;
        }

        // 带重试的启动
        bool success = false;
        int attempts = 0;
        while (attempts <= max_retries_ && running_.load()) {
            attempts++;
            {
                std::lock_guard<std::mutex> lock(states_mutex_);
                NodeRuntimeState& state = states_[inst_name];
                state.status = NodeRuntimeStatus::STARTING;
                if (status_cb_) status_cb_(inst_name, state.status, "");
                LOG_INFO("[DagLauncher] 启动 %s (尝试 %d/%d)",
                         inst_name.c_str(), attempts, max_retries_ + 1);

                if (launchNode(cfg_it->second, state)) {
                    state.status = NodeRuntimeStatus::RUNNING;
                    if (status_cb_) status_cb_(inst_name, state.status, "");
                    LOG_INFO("[DagLauncher] %s PID=%d", inst_name.c_str(), state.pid);
                    success = true;
                    break;
                } else {
                    state.status = NodeRuntimeStatus::FAILED;
                    if (status_cb_) status_cb_(inst_name, state.status, state.error_msg);
                    LOG_WARN("[DagLauncher] %s 启动失败: %s",
                             inst_name.c_str(), state.error_msg.c_str());
                }
            }
            // 失败后等待再重试
            if (attempts <= max_retries_ && running_.load()) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(retry_delay_ms_));
            }
        }

        if (!success) {
            LOG_ERROR("[DagLauncher] %s 最终启动失败 (已重试 %d 次)",
                      inst_name.c_str(), max_retries_);
            all_ok = false;
        }

        // 节点间启动间隔
        if (startup_delay_ms > 0 && running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(startup_delay_ms));
        }
    }

    return all_ok;
}

// ========== launchParallel: 按层并行启动 ==========
bool DagLauncher::launchParallel(int inter_layer_delay_ms) {
    auto layers = getStartupLayers();
    if (layers.empty()) {
        LOG_ERROR("[DagLauncher] 启动层为空（可能存在环）");
        return false;
    }

    running_ = true;
    monitor_thread_ = std::make_unique<std::thread>([this]() {
        this->monitorThreadFunc();
    });

    bool all_ok = true;
    LOG_INFO("[DagLauncher] 按 %zu 层并行启动", layers.size());

    for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
        const auto& layer = layers[layer_idx];
        if (!running_.load()) break;

        LOG_INFO("[DagLauncher] 第 %zu 层: %zu 个节点并行启动",
                 layer_idx, layer.size());

        std::vector<std::future<bool>> futures;
        futures.reserve(layer.size());

        // 对同层每个节点，启动独立线程来 fork
        for (const auto& inst_name : layer) {
            futures.push_back(std::async(std::launch::async, [this, &inst_name]() {
                auto cfg_it = launch_configs_.find(inst_name);
                if (cfg_it == launch_configs_.end()) {
                    LOG_ERROR("[DagLauncher] 找不到配置: %s", inst_name.c_str());
                    return false;
                }
                if (!fs::exists(cfg_it->second.binary_path)) {
                    LOG_WARN("[DagLauncher] binary 不存在: %s",
                             cfg_it->second.binary_path.c_str());
                    return true;  // 跳过（但不视为失败）
                }

                // 带重试的启动
                for (int attempt = 0; attempt <= max_retries_; ++attempt) {
                    {
                        std::lock_guard<std::mutex> lock(states_mutex_);
                        NodeRuntimeState& state = states_[inst_name];
                        state.status = NodeRuntimeStatus::STARTING;
                        if (status_cb_) status_cb_(inst_name, state.status, "");
                        LOG_INFO("[DagLauncher] 启动 %s (尝试 %d/%d)",
                                 inst_name.c_str(), attempt + 1, max_retries_ + 1);

                        if (launchNode(cfg_it->second, state)) {
                            state.status = NodeRuntimeStatus::RUNNING;
                            if (status_cb_) status_cb_(inst_name, state.status, "");
                            LOG_INFO("[DagLauncher] %s PID=%d", inst_name.c_str(), state.pid);
                            return true;
                        } else {
                            state.status = NodeRuntimeStatus::FAILED;
                            LOG_WARN("[DagLauncher] %s 启动失败: %s",
                                     inst_name.c_str(), state.error_msg.c_str());
                        }
                    }
                    if (attempt < max_retries_) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(retry_delay_ms_));
                    }
                }
                return false;
            }));
        }

        // 等待本层全部完成
        for (auto& f : futures) {
            if (!f.get()) all_ok = false;
        }

        // 层间等待
        if (inter_layer_delay_ms > 0 && running_.load()
            && layer_idx + 1 < layers.size()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(inter_layer_delay_ms));
        }
    }

    return all_ok;
}

bool DagLauncher::launchNode(const NodeLaunchConfig& config, NodeRuntimeState& state) {
    std::vector<std::string> args;
    args.push_back(config.binary_path);
    if (!config.config_file.empty()) {
        args.push_back("--config");
        args.push_back(config.config_file);
    }
    args.push_back("--instance");
    args.push_back(config.instance_name);
    if (!config.topic_map.empty()) {
        args.push_back("--topic-map");
        args.push_back(config.topic_map);
    }
    if (!system_config_file_.empty()) {
        args.push_back("--system-config");
        args.push_back(system_config_file_);
    }

    for (const auto& a : args) {
        LOG_INFO("  %s", a.c_str());
    }

    pid_t pid = fork();
    if (pid < 0) {
        state.status = NodeRuntimeStatus::FAILED;
        state.error_msg = std::string("fork failed: ") + std::strerror(errno);
        LOG_ERROR("[DagLauncher] fork 失败: %s", state.error_msg.c_str());
        return false;
    }

    if (pid == 0) {
        fs::path binary_dir = fs::path(config.binary_path).parent_path();
        if (!binary_dir.empty()) {
            chdir(binary_dir.c_str());
        }
        
        std::vector<char*> c_args;
        c_args.reserve(args.size() + 1);
        for (auto& a : args) {
            c_args.push_back(const_cast<char*>(a.c_str()));
        }
        c_args.push_back(nullptr);
        execv(c_args[0], c_args.data());
        std::cerr << "[DagLauncher] execv 失败: " << c_args[0]
                  << " (" << std::strerror(errno) << ")" << std::endl;
        _exit(1);
    }

    state.pid = pid;
    state.last_start_time = std::chrono::steady_clock::now();
    return true;
}

bool DagLauncher::allRunning() const {
    std::lock_guard<std::mutex> lock(states_mutex_);
    for (const auto& [name, state] : states_) {
        if (state.status != NodeRuntimeStatus::RUNNING) return false;
    }
    return !states_.empty();
}

NodeRuntimeStatus DagLauncher::getStatus(const std::string& instance_name) const {
    std::lock_guard<std::mutex> lock(states_mutex_);
    auto it = states_.find(instance_name);
    if (it == states_.end()) return NodeRuntimeStatus::PENDING;
    return it->second.status;
}

void DagLauncher::monitorThreadFunc() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 1) 检测崩溃 / 退出
        std::vector<std::string> candidates_for_restart;
        {
            std::lock_guard<std::mutex> lock(states_mutex_);
            for (auto& [name, state] : states_) {
                if (state.pid <= 0) continue;
                if (state.status != NodeRuntimeStatus::RUNNING) continue;

                int wstatus;
                pid_t result = waitpid(state.pid, &wstatus, WNOHANG);
                if (result > 0) {
                    if (WIFEXITED(wstatus)) {
                        int exit_code = WEXITSTATUS(wstatus);
                        state.exit_code = exit_code;
                        state.status = (exit_code == 0)
                                           ? NodeRuntimeStatus::EXITED
                                           : NodeRuntimeStatus::CRASHED;
                        state.error_msg = state.status == NodeRuntimeStatus::CRASHED
                                              ? "exit_code=" + std::to_string(exit_code)
                                              : "";
                        LOG_WARN("[DagLauncher] %s 已退出 (code=%d)", name.c_str(), exit_code);
                    } else if (WIFSIGNALED(wstatus)) {
                        state.status = NodeRuntimeStatus::CRASHED;
                        state.error_msg = "signaled=" + std::to_string(WTERMSIG(wstatus));
                        LOG_WARN("[DagLauncher] %s 被信号终止", name.c_str());
                    }
                    state.pid = 0;
                    if (status_cb_) status_cb_(name, state.status, state.error_msg);

                    // CRASHED -> 加入重启候选
                    if (state.status == NodeRuntimeStatus::CRASHED
                        && auto_restart_
                        && state.restart_count < max_restarts_) {
                        candidates_for_restart.push_back(name);
                    }
                }
            }
        }

        // 2) 执行重启（在锁外执行，避免长时间持有）
        for (const auto& name : candidates_for_restart) {
            if (!running_.load()) break;
            LOG_INFO("[DagLauncher] 等待 %dms 后自动重启 %s",
                     auto_restart_delay_ms_, name.c_str());
            std::this_thread::sleep_for(
                std::chrono::milliseconds(auto_restart_delay_ms_));
            restartNode(name);
        }
    }
}

void DagLauncher::waitForAll() {
    while (running_.load()) {
        bool any_running = false;
        {
            std::lock_guard<std::mutex> lock(states_mutex_);
            for (const auto& [name, state] : states_) {
                if (state.pid > 0 && state.status == NodeRuntimeStatus::RUNNING) {
                    any_running = true;
                    break;
                }
            }
        }
        if (!any_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void DagLauncher::stop() {
    if (!running_.load()) {
        LOG_INFO("[DagLauncher] stop() 被重复调用，忽略");
        return;
    }
    
    LOG_INFO("[DagLauncher] 正在停止所有节点...");
    
    // 1) 发送 SIGTERM 给所有运行中的节点
    {
        std::lock_guard<std::mutex> lock(states_mutex_);
        for (auto& [name, state] : states_) {
            if (state.pid > 0 && state.status == NodeRuntimeStatus::RUNNING) {
                LOG_INFO("[DagLauncher] 发送 SIGTERM 到 %s (PID=%d)", 
                         name.c_str(), state.pid);
                kill(state.pid, SIGTERM);
                state.status = NodeRuntimeStatus::PENDING;  // 标记为正在停止
            }
        }
    }
    
    // 2) 等待子进程优雅退出 (超时5秒)
    const int TIMEOUT_MS = 5000;
    const int CHECK_INTERVAL_MS = 100;
    int elapsed_ms = 0;
    
    while (elapsed_ms < TIMEOUT_MS) {
        bool all_exited = true;
        
        {
            std::lock_guard<std::mutex> lock(states_mutex_);
            for (auto& [name, state] : states_) {
                if (state.pid > 0) {
                    // 检查进程是否已退出
                    int wstatus;
                    pid_t result = waitpid(state.pid, &wstatus, WNOHANG);
                    if (result > 0) {
                        // 进程已退出，回收
                        state.pid = 0;
                        if (WIFEXITED(wstatus)) {
                            state.exit_code = WEXITSTATUS(wstatus);
                            state.status = (state.exit_code == 0) 
                                           ? NodeRuntimeStatus::EXITED 
                                           : NodeRuntimeStatus::CRASHED;
                        } else {
                            state.status = NodeRuntimeStatus::EXITED;
                        }
                        LOG_INFO("[DagLauncher] %s 已退出 (code=%d)", name.c_str(), state.exit_code);
                    } else if (result == 0) {
                        // 进程仍在运行
                        all_exited = false;
                    } else {
                        // waitpid 错误，进程可能已不存在
                        state.pid = 0;
                        state.status = NodeRuntimeStatus::EXITED;
                    }
                }
            }
        }
        
        if (all_exited) {
            LOG_INFO("[DagLauncher] 所有节点已优雅退出 (%dms)", elapsed_ms);
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL_MS));
        elapsed_ms += CHECK_INTERVAL_MS;
    }
    
    // 3) 超时后强制 kill
    if (elapsed_ms >= TIMEOUT_MS) {
        LOG_WARN("[DagLauncher] 等待超时 (%dms)，强制终止剩余节点", TIMEOUT_MS);
        std::lock_guard<std::mutex> lock(states_mutex_);
        for (auto& [name, state] : states_) {
            if (state.pid > 0) {
                LOG_WARN("[DagLauncher] 强制 kill %s (PID=%d)", 
                         name.c_str(), state.pid);
                kill(state.pid, SIGKILL);
                
                // 等待回收
                int wstatus;
                waitpid(state.pid, &wstatus, 0);
                state.pid = 0;
                state.status = NodeRuntimeStatus::EXITED;
            }
        }
    }
    
    running_ = false;
    LOG_INFO("[DagLauncher] 所有节点已停止");
}

void DagLauncher::forceKill() {
    LOG_WARN("[DagLauncher] 强制 kill 所有节点");
    std::lock_guard<std::mutex> lock(states_mutex_);
    for (auto& [name, state] : states_) {
        if (state.pid > 0) {
            kill(state.pid, SIGKILL);
            int wstatus;
            waitpid(state.pid, &wstatus, 0);
        }
    }
    running_ = false;
}

std::map<std::string, std::vector<std::string>> DagLauncher::getRoleProviders() const {
    std::map<std::string, std::vector<std::string>> result;
    const auto& templates = scheduler_.templates();
    const auto& instances = scheduler_.instances();
    for (const auto& [name, inst] : instances) {
        auto tit = templates.find(inst.template_name);
        if (tit == templates.end()) continue;
        for (const auto& svc : tit->second.services) {
            result[svc.role].push_back(name);
        }
    }
    return result;
}

std::map<std::string, std::vector<std::string>> DagLauncher::getRoleRequirements() const {
    std::map<std::string, std::vector<std::string>> result;
    const auto& templates = scheduler_.templates();
    const auto& instances = scheduler_.instances();
    for (const auto& [name, inst] : instances) {
        auto tit = templates.find(inst.template_name);
        if (tit == templates.end()) continue;
        for (const auto& req_role : tit->second.requires_services) {
            result[name].push_back(req_role);
        }
    }
    return result;
}

// ========== Phase 3: 自动重启、动态热更新、资源监控 ==========

bool DagLauncher::restartNode(const std::string& instance_name) {
    NodeLaunchConfig cfg;
    {
        std::lock_guard<std::mutex> lock(states_mutex_);
        auto it = launch_configs_.find(instance_name);
        if (it == launch_configs_.end()) {
            LOG_ERROR("[DagLauncher] 找不到要重启的节点配置: %s",
                      instance_name.c_str());
            return false;
        }
        cfg = it->second;

        NodeRuntimeState& state = states_[instance_name];
        if (state.restart_count >= max_restarts_) {
            LOG_WARN("[DagLauncher] %s 已超过最大重启次数 %d，停止重启",
                     instance_name.c_str(), max_restarts_);
            return false;
        }
    }

    LOG_INFO("[DagLauncher] 重启 %s (第 %d 次重启)",
             instance_name.c_str(), states_[instance_name].restart_count + 1);

    {
        std::lock_guard<std::mutex> lock(states_mutex_);
        NodeRuntimeState& state = states_[instance_name];
        state.status = NodeRuntimeStatus::STARTING;
        if (status_cb_) status_cb_(instance_name, state.status, "");

        if (!launchNode(cfg, state)) {
            state.status = NodeRuntimeStatus::FAILED;
            state.error_msg = "restart failed";
            if (status_cb_) status_cb_(instance_name, state.status, state.error_msg);
            return false;
        }
        state.status = NodeRuntimeStatus::RUNNING;
        state.restart_count++;
        if (status_cb_) status_cb_(instance_name, state.status, "");
        LOG_INFO("[DagLauncher] %s 重启成功，PID=%d，重启次数=%d",
                 instance_name.c_str(), state.pid, state.restart_count);
    }
    return true;
}

bool DagLauncher::stopNode(const std::string& instance_name, int signal) {
    std::lock_guard<std::mutex> lock(states_mutex_);
    auto it = states_.find(instance_name);
    if (it == states_.end()) return false;
    NodeRuntimeState& state = it->second;
    if (state.pid > 0) {
        kill(state.pid, signal);
        int wstatus;
        waitpid(state.pid, &wstatus, 0);
        state.pid = 0;
        state.status = NodeRuntimeStatus::PENDING;
        LOG_INFO("[DagLauncher] 已停止 %s (signal=%d)",
                 instance_name.c_str(), signal);
        return true;
    }
    return false;
}

bool DagLauncher::reloadPipeline(const std::string& new_pipeline_path, PipelineDiff& diff) {
    const std::string target_path = new_pipeline_path.empty() ? pipeline_path_ : new_pipeline_path;

    LOG_INFO("[DagLauncher] 重新加载 pipeline: %s", target_path.c_str());

    DagScheduler new_scheduler;
    if (!new_scheduler.loadFromXml(target_path)) {
        LOG_ERROR("[DagLauncher] 加载新的 pipeline.xml 失败");
        return false;
    }

    std::string error_msg;
    if (!new_scheduler.validate(error_msg)) {
        LOG_ERROR("[DagLauncher] 新的 pipeline 校验失败:\n%s", error_msg.c_str());
        return false;
    }

    diff = computeDiff(new_scheduler);
    if (!diff.changed) {
        LOG_INFO("[DagLauncher] pipeline 未变化，无操作");
        return true;
    }

    // 停止：to_stop（不再需要的节点） + to_restart（配置变更的节点）
    for (const auto& name : diff.to_stop) {
        LOG_INFO("[DagLauncher] [热更新] 停止 %s", name.c_str());
        stopNode(name);
    }
    for (const auto& name : diff.to_restart) {
        LOG_INFO("[DagLauncher] [热更新] 重启 %s", name.c_str());
        stopNode(name);
    }

    // 切换到新的 scheduler + bins_dir
    scheduler_ = std::move(new_scheduler);
    if (!new_pipeline_path.empty()) pipeline_path_ = new_pipeline_path;

    // 重新构建 launch_configs + states（保留未变更节点的运行状态）
    auto order = scheduler_.computeStartupOrder();
    for (const auto& inst : order) {
        launch_configs_[inst] = buildConfig(inst);
        if (states_.find(inst) == states_.end()) {
            states_[inst] = NodeRuntimeState();
        }
    }

    // 启动 to_restart（按层重新启动）
    for (const auto& name : diff.to_restart) {
        NodeRuntimeState& state = states_[name];
        auto cfg_it = launch_configs_.find(name);
        if (cfg_it == launch_configs_.end()) continue;
        state.status = NodeRuntimeStatus::STARTING;
        if (status_cb_) status_cb_(name, state.status, "");
        if (launchNode(cfg_it->second, state)) {
            state.status = NodeRuntimeStatus::RUNNING;
            if (status_cb_) status_cb_(name, state.status, "");
        } else {
            state.status = NodeRuntimeStatus::FAILED;
            if (status_cb_) status_cb_(name, state.status, state.error_msg);
        }
    }

    // 启动 to_start（新的节点）
    for (const auto& name : diff.to_start) {
        NodeRuntimeState& state = states_[name];
        auto cfg_it = launch_configs_.find(name);
        if (cfg_it == launch_configs_.end()) continue;
        state.status = NodeRuntimeStatus::STARTING;
        if (status_cb_) status_cb_(name, state.status, "");
        if (launchNode(cfg_it->second, state)) {
            state.status = NodeRuntimeStatus::RUNNING;
            if (status_cb_) status_cb_(name, state.status, "");
        } else {
            state.status = NodeRuntimeStatus::FAILED;
            if (status_cb_) status_cb_(name, state.status, state.error_msg);
        }
    }

    LOG_INFO("[DagLauncher] 热更新完成: 停止=%zu, 重启=%zu, 新增=%zu",
             diff.to_stop.size(), diff.to_restart.size(), diff.to_start.size());
    return true;
}

PipelineDiff DagLauncher::computeDiff(const DagScheduler& new_scheduler) const {
    PipelineDiff diff;
    const auto& new_instances = new_scheduler.instances();
    const auto& old_instances = scheduler_.instances();

    std::set<std::string> old_names, new_names;
    for (const auto& [name, _] : old_instances) old_names.insert(name);
    for (const auto& [name, _] : new_instances) new_names.insert(name);

    // 旧的但不在新配置中的 -> 停止
    for (const auto& name : old_names) {
        if (new_names.find(name) == new_names.end()) {
            diff.to_stop.push_back(name);
        }
    }

    // 新的但不在旧配置中的 -> 启动
    for (const auto& name : new_names) {
        if (old_names.find(name) == old_names.end()) {
            diff.to_start.push_back(name);
        }
    }

    // 都存在但配置改变的 -> 重启（基于 config_file 比较
    for (const auto& [name, new_inst] : new_instances) {
        auto old_it = old_instances.find(name);
        if (old_it == old_instances.end()) continue;
        // config 相同名称也需要重新配置变更了
        if (new_inst.config_file != old_it->second.config_file) {
            diff.to_restart.push_back(name);
        }
    }

    diff.changed = !diff.to_stop.empty() || !diff.to_restart.empty() || !diff.to_start.empty();
    return diff;
}

NodeResourceUsage DagLauncher::getResourceUsage(const std::string& instance_name) const {
    NodeResourceUsage usage;
    int pid = 0;
    {
        std::lock_guard<std::mutex> lock(states_mutex_);
        auto it = states_.find(instance_name);
        if (it == states_.end()) return usage;
        pid = it->second.pid;
    }
    if (pid <= 0) return usage;

    // 从 /proc/<pid>/statm 读取内存
    std::ifstream statm("/proc/" + std::to_string(pid) + "/statm");
    if (statm.is_open()) {
        long size = 0, resident = 0;
        if (statm >> size >> resident);
        statm.close();
        long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024;
        usage.memory_kb = resident * page_size_kb;
        usage.vm_kb = size * page_size_kb;
    }

    // 从 /proc/<pid>/stat 读取 CPU ticks
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    if (stat.is_open()) {
        std::string line;
        std::getline(stat, line);
        stat.close();
        // utime 13, stime 14, cutime 15, cstime 16, starttime 22
        size_t pos = 0;
        int field = 0;
        long utime = 0, stime = 0, starttime = 0;
        while (line.size() > 0 && field < 22) {
            size_t next = line.find(')', pos);
            if (next == std::string::npos) break;
            field++;
            if (field == 13) utime = std::atol(line.substr(pos, next - pos).c_str());
            if (field == 14) stime = std::atol(line.substr(pos, next - pos).c_str());
            if (field == 22) starttime = std::atol(line.substr(pos, next - pos).c_str());
            pos = next + 1;
        }
        long ticks = utime + stime;
        long hz = sysconf(_SC_CLK_TCK);
        long uptime_sec = (starttime / hz);
        if (hz > 0) {
            long elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() -
                std::chrono::steady_clock::now()).count();
        }
    }
    return usage;
}

std::map<std::string, NodeResourceUsage> DagLauncher::getAllResourceUsage() const {
    std::map<std::string, NodeResourceUsage> result;
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(states_mutex_);
        for (const auto& [name, _] : states_) names.push_back(name);
    }
    for (const auto& name : names) {
        result[name] = getResourceUsage(name);
    }
    return result;
}

std::string DagLauncher::getStatusReport() const {
    std::ostringstream oss;
    std::lock_guard<std::mutex> lock(states_mutex_);
    oss << "=== DagLauncher Status Report (" << states_.size() << " nodes) "
         << " ===" << std::endl;
    for (const auto& [name, state] : states_) {
        const char* s = "UNKNOWN";
        switch (state.status) {
            case NodeRuntimeStatus::PENDING:  s = "PENDING"; break;
            case NodeRuntimeStatus::STARTING: s = "STARTING"; break;
            case NodeRuntimeStatus::RUNNING:  s = "RUNNING"; break;
            case NodeRuntimeStatus::EXITED:   s = "EXITED"; break;
            case NodeRuntimeStatus::CRASHED:  s = "CRASHED"; break;
            case NodeRuntimeStatus::FAILED:   s = "FAILED"; break;
        }
        oss << "  " << name << ": status=" << s
            << " pid=" << state.pid
            << " restarts=" << state.restart_count
            << " exit=" << state.exit_code
            << std::endl;
    }
    return oss.str();
}
