#include "ros2_backend.h"

#ifdef HAS_ROS2
#include <rclcpp/rclcpp.hpp>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <iostream>

namespace ros2_global {

static std::shared_ptr<rclcpp::Node> g_node;
static std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> g_executor;
static std::thread g_spin_thread;
static std::atomic<bool> g_running{false};
static std::once_flag g_init_flag;

void init(const std::string& node_name) {
    std::call_once(g_init_flag, [&node_name]() {
        // 初始化 rclcpp（仅首次调用生效）
        if (!rclcpp::ok()) {
            rclcpp::init(0, nullptr);
        }

        // 创建节点
        rclcpp::NodeOptions options;
        options.use_intra_process_comms(true);
        g_node = std::make_shared<rclcpp::Node>(node_name, options);

        // 创建 executor 并添加节点
        g_executor = std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
        g_executor->add_node(g_node);

        // 在后台线程中 spin
        g_running.store(true);
        g_spin_thread = std::thread([]() {
            while (g_running.load() && rclcpp::ok()) {
                try {
                    g_executor->spin_some(std::chrono::milliseconds(50));
                } catch (const std::exception& e) {
                    std::cerr << "[ROS2] spin_some error: " << e.what() << std::endl;
                }
            }
        });

        std::cout << "[ROS2] 节点 '" << node_name << "' 已初始化" << std::endl;
    });
}

void shutdown() {
    if (!g_running.exchange(false)) {
        return;  // 已经关闭
    }

    // 停止 executor
    if (g_executor) {
        g_executor->cancel();
    }

    // 等待 spin 线程退出
    if (g_spin_thread.joinable()) {
        g_spin_thread.join();
    }

    // 清除节点和 executor
    g_node.reset();
    g_executor.reset();

    // 关闭 rclcpp（如果有其他节点可能还在使用，需谨慎）
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
