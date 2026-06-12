#include "tcp_client.h"
#include "logger/logger.h"
#include <cstring>
#include <chrono>

// ========== 跨平台辅助函数 ==========

bool platformSocketInit() {
#ifdef _WIN32
    WSADATA wsa;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (ret != 0) {
        LOG_ERROR("[错误] WSAStartup 失败, 错误码: %d", ret);
        return false;
    }
#endif
    return true;
}

void platformSocketCleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

std::string getSocketError() {
#ifdef _WIN32
    int err = WSAGetLastError();
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, buf, sizeof(buf), nullptr);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    return std::string(buf) + " (code=" + std::to_string(err) + ")";
#else
    return strerror(errno);
#endif
}

// ========== TcpClient 实现 ==========

TcpClient::TcpClient(const std::string& host, int port)
    : host_(host), port_(port) {}

TcpClient::~TcpClient() {
    disconnect();
}

bool TcpClient::connect() {
    std::lock_guard<std::mutex> lock(connect_mutex_);

    if (connected_.load()) {
        return true;
    }

    sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ == INVALID_SOCK) {
        LOG_ERROR("[客户端] 创建 socket 失败: %s", getSocketError().c_str());
        return false;
    }

    // macOS: 防止 SIGPIPE
#if defined(__APPLE__)
    int nosigpipe = 1;
    setsockopt(sock_fd_, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port_));

    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        LOG_ERROR("[客户端] 无效的服务器地址: %s", host_.c_str());
        closeSocket();
        return false;
    }

    if (::connect(sock_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR("[客户端] 连接失败 (%s:%d): %s", host_.c_str(), port_, getSocketError().c_str());
        closeSocket();
        return false;
    }

    connected_.store(true);
    LOG_INFO("[客户端] 已连接到服务器: %s:%d", host_.c_str(), port_);

    if (connect_callback_) {
        connect_callback_(true);
    }

    return true;
}

void TcpClient::disconnect() {
    running_.store(false);

    if (reconnect_thread_.joinable()) {
        reconnect_thread_.join();
    }

    closeSocket();

    if (connected_.load()) {
        connected_.store(false);
        if (connect_callback_) {
            connect_callback_(false);
        }
    }

    LOG_INFO("[客户端] 已断开连接");
}

bool TcpClient::isConnected() const {
    return connected_.load();
}

bool TcpClient::send(const std::string& data) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    if (!connected_.load() || sock_fd_ == INVALID_SOCK) {
        return false;
    }

    int flags = 0;
#if defined(__linux__)
    flags = MSG_NOSIGNAL;
#endif

    int sent = ::send(sock_fd_, data.c_str(), static_cast<int>(data.size()), flags);
    if (sent <= 0) {
        LOG_ERROR("[客户端] 发送失败: %s", getSocketError().c_str());

        // 标记断连
        connected_.store(false);
        closeSocket();

        if (connect_callback_) {
            connect_callback_(false);
        }

        return false;
    }

    return true;
}

void TcpClient::enableAutoReconnect(bool enable, int intervalMs) {
    auto_reconnect_.store(enable);
    reconnect_interval_ms_ = intervalMs;

    if (enable && !reconnect_thread_.joinable()) {
        reconnect_thread_ = std::thread(&TcpClient::reconnectLoop, this);
    }
}

void TcpClient::setConnectCallback(std::function<void(bool connected)> callback) {
    connect_callback_ = callback;
}

void TcpClient::reconnectLoop() {
    while (running_.load()) {
        if (!connected_.load() && auto_reconnect_.load()) {
            LOG_INFO("[客户端] 尝试重连 %s:%d ...", host_.c_str(), port_);
            connect();
        }

        // 分段 sleep，以便更快响应 disconnect()
        for (int i = 0; i < reconnect_interval_ms_ / 100 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void TcpClient::closeSocket() {
    if (sock_fd_ != INVALID_SOCK) {
#ifdef _WIN32
        shutdown(sock_fd_, SD_BOTH);
#else
        shutdown(sock_fd_, SHUT_RDWR);
#endif
        CLOSE_SOCKET(sock_fd_);
        sock_fd_ = INVALID_SOCK;
    }
}
