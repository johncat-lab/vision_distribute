#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

// ========== 跨平台 socket 兼容层 ==========
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET socket_t;
    #define INVALID_SOCK INVALID_SOCKET
    #define CLOSE_SOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    typedef int socket_t;
    #define INVALID_SOCK (-1)
    #define CLOSE_SOCKET close
#endif

// Winsock 初始化/清理 (仅 Windows 需要)
bool platformSocketInit();
void platformSocketCleanup();

// 跨平台获取最近的 socket 错误描述
std::string getSocketError();

class TcpClient {
public:
    TcpClient(const std::string& host, int port);
    ~TcpClient();

    // 禁止拷贝
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // 连接到服务器
    bool connect();

    // 断开连接
    void disconnect();

    // 是否已连接
    bool isConnected() const;

    // 发送数据到服务器
    bool send(const std::string& data);

    // 启用/禁用自动重连
    void enableAutoReconnect(bool enable, int intervalMs = 3000);

    // 设置连接状态变化回调
    void setConnectCallback(std::function<void(bool connected)> callback);

private:
    void reconnectLoop();
    void closeSocket();

    std::string host_;
    int port_;
    socket_t sock_fd_ = INVALID_SOCK;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{true};
    std::atomic<bool> auto_reconnect_{false};
    int reconnect_interval_ms_ = 3000;

    std::thread reconnect_thread_;
    std::mutex send_mutex_;
    std::mutex connect_mutex_;

    std::function<void(bool)> connect_callback_;
};

#endif // TCP_CLIENT_H
