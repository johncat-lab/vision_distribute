#pragma once
#include <string>
#include <functional>
#include <future>

template<typename Request, typename Response>
class IService {
public:
    virtual ~IService() = default;
    using Handler = std::function<Response(const Request&)>;
    virtual bool serve(const std::string& endpoint, Handler handler) = 0;
    virtual Response call(const std::string& endpoint, const Request& req) = 0;
    virtual void preconnect() = 0;  // 预连接：提前创建 client，避免首次调用延迟
    
    /// @brief 异步调用 Service (默认实现抛出异常，子类可重写)
    /// @param endpoint 端点名
    /// @param req 请求
    /// @return std::future<Response> 异步结果
    virtual std::future<Response> call_async(const std::string& endpoint, const Request& req) {
        throw std::runtime_error("call_async not implemented");
    }
};