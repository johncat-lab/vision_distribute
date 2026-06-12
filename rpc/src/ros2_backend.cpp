#include "ros2_backend.h"
#include "logger/logger.h"

#ifdef HAS_ROS2
#include <rclcpp/rclcpp.hpp>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <chrono>

namespace ros2_global {

static std::shared_ptr<rclcpp::Node> g_node;
static std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> g_executor;
static std::thread g_spin_thread;
static std::atomic<bool> g_running{false};
static std::mutex g_init_mutex;
static bool g_initialized = false;

void init(const std::string& node_name, int executor_threads) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialized) return;

    if (!rclcpp::ok()) {
        rclcpp::init(0, nullptr);
    }

    rclcpp::NodeOptions options;
    options.use_intra_process_comms(false);
    g_node = std::make_shared<rclcpp::Node>(node_name, options);

    int threads = (executor_threads > 0) ? executor_threads : 4;
    g_executor = std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
        rclcpp::ExecutorOptions{}, threads);
    g_executor->add_node(g_node);

    g_initialized = true;
    LOG_INFO("[ROS2] 节点 '%s' 已初始化, executor 线程数=%d", node_name.c_str(), threads);
}

void start_executor() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (!g_initialized) return;
    if (g_running.load()) return;

    g_running.store(true);
    LOG_INFO("[ROS2] 启动 executor 线程...");
    g_spin_thread = std::thread([]() {
        LOG_DEBUG("[ROS2] executor 线程已启动，开始 spin...");
        g_executor->spin();
        LOG_DEBUG("[ROS2] executor spin 结束");
    });

    // 等待 executor 线程启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (!g_initialized) return;

    g_running.store(false);

    if (g_executor) {
        g_executor->cancel();
    }

    if (g_spin_thread.joinable()) {
        g_spin_thread.join();
    }

    g_node.reset();
    g_executor.reset();
    g_initialized = false;

    if (rclcpp::ok()) {
        rclcpp::shutdown();
    }

    LOG_INFO("[ROS2] 节点已关闭");
}

rclcpp::Node* getNode() {
    return g_node.get();
}

}  // namespace ros2_global

#else
// 非 ROS2 构建时，cpp 文件为空（存根已在 header 中定义）
#endif  // HAS_ROS2
