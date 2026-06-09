#include "ros2_backend.h"

#ifdef HAS_ROS2
#include <rclcpp/rclcpp.hpp>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <iostream>
#include <chrono>

namespace ros2_global {

static std::shared_ptr<rclcpp::Node> g_node;
static std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> g_executor;
static std::thread g_spin_thread;
static std::atomic<bool> g_running{false};
static std::mutex g_init_mutex;
static bool g_initialized = false;

void init(const std::string& node_name) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialized) return;

    if (!rclcpp::ok()) {
        rclcpp::init(0, nullptr);
    }

    rclcpp::NodeOptions options;
    options.use_intra_process_comms(false);
    g_node = std::make_shared<rclcpp::Node>(node_name, options);

    g_executor = std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
        rclcpp::ExecutorOptions{}, 2);
    g_executor->add_node(g_node);

    g_running.store(true);
    g_spin_thread = std::thread([]() {
        std::cerr << "[ROS2] spin thread started, tid="
                  << std::this_thread::get_id() << std::endl;
        try {
            g_executor->spin();
        } catch (const std::exception& e) {
            std::cerr << "[ROS2] executor spin error: " << e.what() << std::endl;
        }
        std::cerr << "[ROS2] spin thread exited" << std::endl;
    });

    g_initialized = true;
    std::cout << "[ROS2] 节点 '" << node_name << "' 已初始化" << std::endl;
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

    std::cout << "[ROS2] 节点已关闭" << std::endl;
}

rclcpp::Node* getNode() {
    return g_node.get();
}

}  // namespace ros2_global

#else
// 非 ROS2 构建时，cpp 文件为空（存根已在 header 中定义）
#endif  // HAS_ROS2
