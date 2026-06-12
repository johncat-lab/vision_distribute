#include "vision_server.h"
#include "logger/logger.h"
#include <cstring>
#include <chrono>

// ========== VisionServer 实现 ==========

VisionServer::VisionServer(int port, ServerMode mode, const std::string& bind_addr)
    : port_(port), bind_addr_(bind_addr), mode_(mode), result_str_("NG") {}

VisionServer::~VisionServer() {
    stop();
}

bool VisionServer::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ == INVALID_SOCK) {
        LOG_ERROR("[服务器] 创建 socket 失败: %s", getSocketError().c_str());
        return false;
    }

    // 允许端口复用
#ifdef _WIN32
    char opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#else
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (bind_addr_ == "0.0.0.0" || bind_addr_.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr) != 1) {
            LOG_WARN("[服务器] 无效的绑定地址: %s, 使用 INADDR_ANY", bind_addr_.c_str());
            addr.sin_addr.s_addr = INADDR_ANY;
        }
    }
    addr.sin_port = htons(static_cast<unsigned short>(port_));

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR("[服务器] bind 失败 (端口 %d): %s", port_, getSocketError().c_str());
        CLOSE_SOCKET(server_fd_);
        server_fd_ = INVALID_SOCK;
        return false;
    }

    if (listen(server_fd_, 5) != 0) {
        LOG_ERROR("[服务器] listen 失败: %s", getSocketError().c_str());
        CLOSE_SOCKET(server_fd_);
        server_fd_ = INVALID_SOCK;
        return false;
    }

    running_ = true;

    const char* mode_name[] = {"连接即发送", "收到请求再发送", "持续周期发送"};
    LOG_INFO("[服务器] TCP Server 启动, 绑定: %s, 端口: %d, 模式: %s", 
             bind_addr_.c_str(), port_, mode_name[static_cast<int>(mode_)]);

    accept_thread_ = std::thread(&VisionServer::acceptLoop, this);
    return true;
}

void VisionServer::stop() {
    running_ = false;

    // 先关闭所有客户端 socket，解除 recv() 阻塞
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (socket_t client_fd : active_clients_) {
#ifdef _WIN32
            shutdown(client_fd, SD_BOTH);
#else
            shutdown(client_fd, SHUT_RDWR);
#endif
        }
    }

    // 关闭服务器监听 socket，解除 accept() 阻塞
    if (server_fd_ != INVALID_SOCK) {
#ifdef _WIN32
        shutdown(server_fd_, SD_BOTH);
#else
        shutdown(server_fd_, SHUT_RDWR);
#endif
        CLOSE_SOCKET(server_fd_);
        server_fd_ = INVALID_SOCK;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (auto& t : client_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        client_threads_.clear();
    }

    // 确保所有客户端 socket 已关闭
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (socket_t client_fd : active_clients_) {
            CLOSE_SOCKET(client_fd);
        }
        active_clients_.clear();
    }

    LOG_INFO("[服务器] TCP Server 已停止");
}

void VisionServer::setInterval(int ms) {
    interval_ms_ = ms;
}

void VisionServer::updateResult(const std::string& result) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    result_str_ = result;
}

void VisionServer::broadcast(const std::string& msg) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    if (active_clients_.empty()) {
        return;
    }

    int flags = 0;
#if defined(__linux__)
    flags = MSG_NOSIGNAL;
#endif

    for (socket_t client_fd : active_clients_) {
        int sent = ::send(client_fd, msg.c_str(), static_cast<int>(msg.size()), flags);
        if (sent < 0) {
            // 发送失败不中断，客户端会在各自线程中被清理
        }
    }
}

size_t VisionServer::getClientCount() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return active_clients_.size();
}

void VisionServer::acceptLoop() {
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        socket_t client_fd = accept(server_fd_,
                                    reinterpret_cast<sockaddr*>(&client_addr),
                                    &client_len);
        if (client_fd == INVALID_SOCK) {
            if (running_) {
                LOG_ERROR("[服务器] accept 失败: %s", getSocketError().c_str());
            }
            continue;
        }

        // macOS: 防止 SIGPIPE
#if defined(__APPLE__)
        int nosigpipe = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

        char ip_buf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
        std::string addr_str = std::string(ip_buf)
                             + ":" + std::to_string(ntohs(client_addr.sin_port));
        LOG_INFO("[服务器] 客户端已连接: %s", addr_str.c_str());

        std::lock_guard<std::mutex> lock(threads_mutex_);
        client_threads_.emplace_back(&VisionServer::handleClient, this, client_fd, addr_str);
    }
}

void VisionServer::handleClient(socket_t client_fd, const std::string& client_addr) {
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        active_clients_.insert(client_fd);
    }

    switch (mode_) {
        case ServerMode::SEND_ON_CONNECT:
            handleSendOnConnect(client_fd, client_addr);
            break;
        case ServerMode::SEND_ON_REQUEST:
            handleSendOnRequest(client_fd, client_addr);
            break;
        case ServerMode::SEND_PERIODIC:
            handleSendPeriodic(client_fd, client_addr);
            break;
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        active_clients_.erase(client_fd);
    }

    CLOSE_SOCKET(client_fd);
    LOG_INFO("[服务器] 客户端已断开: %s", client_addr.c_str());
}

// ========== 模式1: 客户端连接即发送 ==========
void VisionServer::handleSendOnConnect(socket_t client_fd, const std::string& client_addr) {
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        resp = result_str_;
    }

    int flags = 0;
#if defined(__linux__)
    flags = MSG_NOSIGNAL;
#endif

    int sent = ::send(client_fd, resp.c_str(), static_cast<int>(resp.size()), flags);
    if (sent < 0) {
        LOG_ERROR("[服务器] 发送失败 -> %s: %s", client_addr.c_str(), getSocketError().c_str());
    } else {
        LOG_DEBUG("[服务器] 已发送 -> %s: %s", client_addr.c_str(), resp.c_str());
    }
}

// ========== 模式2: 收到请求再发送 ==========
void VisionServer::handleSendOnRequest(socket_t client_fd, const std::string& client_addr) {
    char buf[1024];

    while (running_) {
        memset(buf, 0, sizeof(buf));
        int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            break; // 客户端断开或出错
        }

        std::string request(buf, n);
        LOG_DEBUG("[服务器] 收到请求 <- %s: %s", client_addr.c_str(), request.c_str());

        std::string resp;
        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            resp = result_str_;
        }

        int flags = 0;
#if defined(__linux__)
        flags = MSG_NOSIGNAL;
#endif

        int sent = ::send(client_fd, resp.c_str(), static_cast<int>(resp.size()), flags);
        if (sent < 0) {
            LOG_ERROR("[服务器] 发送失败 -> %s: %s", client_addr.c_str(), getSocketError().c_str());
            break;
        }
        LOG_DEBUG("[服务器] 已响应 -> %s: %s", client_addr.c_str(), resp.c_str());
    }
}

// ========== 模式3: 持续周期性发送 ==========
void VisionServer::handleSendPeriodic(socket_t client_fd, const std::string& client_addr) {
    int flags = 0;
#if defined(__linux__)
    flags = MSG_NOSIGNAL;
#endif

    while (running_) {
        std::string resp;
        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            resp = result_str_;
        }

        int sent = ::send(client_fd, resp.c_str(), static_cast<int>(resp.size()), flags);
        if (sent < 0) {
            LOG_ERROR("[服务器] 发送失败 -> %s: %s", client_addr.c_str(), getSocketError().c_str());
            break;
        }

        // 分段 sleep，以便更快响应 stop()
        for (int i = 0; i < interval_ms_ / 10 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}
