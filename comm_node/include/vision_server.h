#ifndef VISION_SERVER_H
#define VISION_SERVER_H

#include "tcp_client.h"  // 复用跨平台 socket 定义 (socket_t, INVALID_SOCK, CLOSE_SOCKET 等)
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <set>

// TCP Server 工作模式
enum class ServerMode {
    SEND_ON_CONNECT,   // 模式1: 客户端连接即发送最新结果
    SEND_ON_REQUEST,   // 模式2: 收到请求再发送 (推荐工业场景)
    SEND_PERIODIC,     // 模式3: 持续周期性发送
};

class VisionServer {
public:
    VisionServer(int port, ServerMode mode = ServerMode::SEND_ON_REQUEST, const std::string& bind_addr = "0.0.0.0");
    ~VisionServer();

    // 禁止拷贝
    VisionServer(const VisionServer&) = delete;
    VisionServer& operator=(const VisionServer&) = delete;

    // 启动/停止服务器
    bool start();
    void stop();

    bool isRunning() const { return running_.load(); }

    // 设置周期发送间隔 (毫秒), 仅模式3有效
    void setInterval(int ms);

    // 动态更新响应字符串 (线程安全，供推理线程调用)
    void updateResult(const std::string& result);

    // 向所有活跃客户端广播消息
    void broadcast(const std::string& msg);

    // 获取当前已连接的客户端数量
    size_t getClientCount() const;

private:
    void acceptLoop();
    void handleClient(socket_t client_fd, const std::string& client_addr);

    // 各模式的处理逻辑
    void handleSendOnConnect(socket_t client_fd, const std::string& client_addr);
    void handleSendOnRequest(socket_t client_fd, const std::string& client_addr);
    void handleSendPeriodic(socket_t client_fd, const std::string& client_addr);

    int port_;
    std::string bind_addr_;
    ServerMode mode_;
    std::string result_str_;
    mutable std::mutex result_mutex_;

    socket_t server_fd_ = INVALID_SOCK;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    std::vector<std::thread> client_threads_;
    std::mutex threads_mutex_;

    std::set<socket_t> active_clients_;
    mutable std::mutex clients_mutex_;

    int interval_ms_ = 100; // 默认 100ms (工业场景更快)
};

#endif // VISION_SERVER_H
